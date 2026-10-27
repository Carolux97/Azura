//! Downloadable content: installing the player's own packages and starting RAAM's Shadow.
//!
//! A package (an Xbox 360 LIVE file, as the console downloaded it) is installed by the runtime
//! itself (`azura --install_content=<package>`, `runtime/src/dlc/content_install.cpp`): it checks
//! that the package is downloadable content of Gears of War 3 and extracts it into the saves folder,
//! as `0000000000000000/4D5308AB/00000002/<package>/`, where the game finds it on start-up.
//!
//! RAAM's Shadow, the campaign add-on, also needs classes that only the game's Title Update has.
//! Azura declares them in a script patch of its own (see `docs/raams-shadow-dlc.md`), which holds
//! the object indices of the player's GearGame package, so it is made here, from the player's game
//! files, by the runtime (`azura --make_dlc_patch=<folder>`,
//! `runtime/src/dlc/raams_shadow_patch.h`), into `<runtime>/dlc/raams-shadow/`. Its
//! `dlc_patch.json` names the GearGame.xxx it was made from (by SHA-256) and the runtime that made
//! it, and the patch is made again when either changes; a runtime update replaces the runtime
//! folder, so a new runtime always makes its own. The launcher makes it after installing the
//! add-on, and before starting the game when it is missing. With the add-on installed and the patch
//! made, the game starts with the patch as its update volume and with the campaign menu's DLC
//! option enabled.

use std::fs;
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

/// Where the game keeps downloadable content in the saves folder: no profile (all zeros), the
/// game's title ID, Marketplace content (type 2).
const CONTENT_DIR: [&str; 3] = ["0000000000000000", "4D5308AB", "00000002"];

/// Azura's script patch for RAAM's Shadow, made in the runtime folder.
const RAAMS_SHADOW_PATCH: &str = "dlc/raams-shadow";

/// The patch file the game reads from its update volume.
const PATCH_FILE: &str = "GearGame/Patches/ScriptPatch_GearGame.bin";

/// What the patch was made from, beside it.
const PATCH_MANIFEST: &str = "dlc_patch.json";

/// A file only RAAM's Shadow has: its first chapter's level.
const RAAMS_SHADOW_MARKER: &str = "GearGame/CookedXbox360/SP_Bank_P.xxx";

/// IDs of RAAM's Shadow's achievements in its package's achievement database (`spa.bin`, which also
/// holds the game's and other packs' achievements).
pub const RAAMS_SHADOW_ACHIEVEMENTS: std::ops::RangeInclusive<u32> = 56..=65;

/// Installing a package takes a few seconds (it extracts about 2 GB); a runtime still running
/// after this has gone wrong.
const INSTALL_TIMEOUT: Duration = Duration::from_secs(600);

/// Making the patch takes a couple of seconds (it decompresses the GearGame and Engine packages).
const PATCH_TIMEOUT: Duration = Duration::from_secs(120);

/// Gears of War 3's downloadable content, in the order the Installation view lists it (by the key of
/// its name in the interface's texts): the campaign add-on, then the multiplayer packs.
pub const CATALOG: [&str; 5] = ["raamsShadow", "hordeCommandPack", "fenixRising", "forcesOfNature", "versusBooster"];

/// Where a downloadable content of the catalogue stands.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub enum DlcStatus {
    /// Installed and ready to play.
    Ready,
    NotInstalled,
    /// Installed; made ready once the runtime is updated (an older one cannot make the patch).
    NeedsRuntime,
    /// Installed, but its patch could not be made from these game files.
    Unsupported,
    /// A multiplayer pack: Azura does not play them yet, nor tells whether one is installed (the
    /// packages are not identified; the Horde Command Pack's maps are even on the disc).
    Multiplayer,
}

/// A downloadable content of the catalogue and where it stands.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DlcEntry {
    pub id: &'static str,
    pub status: DlcStatus,
}

/// The downloadable content installed in the saves folder.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DlcState {
    /// The catalogue, in order.
    pub entries: Vec<DlcEntry>,
    /// Installed packages the catalogue does not identify.
    pub other_packages: usize,
    /// Folder names of the installed packages.
    pub packages: Vec<String>,
    /// RAAM's Shadow is installed.
    pub raams_shadow: bool,
    /// RAAM's Shadow can be played: it is installed, and its patch is made for these game files.
    pub raams_shadow_playable: bool,
}

/// What the runtime prints after installing a package or making the patch.
#[derive(Debug, Deserialize)]
#[serde(rename_all = "snake_case")]
struct ToolReport {
    ok: bool,
    #[serde(default)]
    display_name: String,
    #[serde(default)]
    error: String,
}

/// What `dlc_patch.json` records: the GearGame.xxx the patch was made from (see
/// `game_files_identity`), and the runtime that made it (its executable's size and modification
/// time).
#[derive(Debug, PartialEq, Eq, Serialize, Deserialize)]
struct PatchManifest {
    #[serde(rename = "GearGame.xxx")]
    gear_game: String,
    runtime: String,
}

fn content_dir(saves: &Path) -> PathBuf {
    CONTENT_DIR.iter().fold(saves.to_path_buf(), |path, part| path.join(part))
}

/// The installed packages' folders.
fn installed_packages(saves: &Path) -> Vec<PathBuf> {
    let Ok(entries) = fs::read_dir(content_dir(saves)) else {
        return Vec::new();
    };
    let mut packages: Vec<PathBuf> = entries.flatten().map(|e| e.path()).filter(|p| p.is_dir()).collect();
    packages.sort();
    packages
}

/// The installed RAAM's Shadow package, if any.
fn raams_shadow(saves: &Path) -> Option<PathBuf> {
    installed_packages(saves).into_iter().find(|package| package.join(RAAMS_SHADOW_MARKER).is_file())
}

/// RAAM's Shadow's achievement database, when the add-on is installed.
pub fn raams_shadow_achievements(saves: &Path) -> Option<PathBuf> {
    raams_shadow(saves).map(|package| package.join("spa.bin")).filter(|spa| spa.is_file())
}

/// Hex SHA-256 of a file.
fn sha256(path: &Path) -> io::Result<String> {
    let mut file = fs::File::open(path)?;
    let mut hasher = Sha256::new();
    let mut buffer = vec![0u8; 1 << 20];
    loop {
        let read = file.read(&mut buffer)?;
        if read == 0 {
            break;
        }
        hasher.update(&buffer[..read]);
    }
    Ok(hasher.finalize().iter().map(|b| format!("{b:02x}")).collect())
}

/// Identifies the game files a patch is made from. A game folder: the SHA-256 of its GearGame.xxx.
/// A disc image (.iso), which the runtime reads in place: the image's size and modification time,
/// as hashing the whole image (7 to 8 GB) every time the launcher checks the patch would take too
/// long, and its GearGame.xxx is not a file of its own.
fn game_files_identity(game: &Path) -> Option<String> {
    if game.is_file() {
        let image = fs::metadata(game).ok()?;
        let modified = image.modified().ok()?.duration_since(std::time::UNIX_EPOCH).ok()?.as_secs();
        return Some(format!("disc image {}:{modified}", image.len()));
    }
    sha256(&game.join("GearGame").join("CookedXbox360").join("GearGame.xxx")).ok()
}

/// What the patch for these game files, made by this runtime, records; `None` when the game files
/// or the runtime executable cannot be read.
fn expected_manifest(runtime: &Path, game: &Path) -> Option<PatchManifest> {
    let gear_game = game_files_identity(game)?;
    let executable = fs::metadata(crate::runtime::executable_path(runtime)).ok()?;
    let modified = executable.modified().ok()?.duration_since(std::time::UNIX_EPOCH).ok()?.as_secs();
    Some(PatchManifest { gear_game, runtime: format!("{}:{modified}", executable.len()) })
}

/// Whether the patch in the runtime folder was made for these game files by this runtime.
fn patch_ready(runtime: &Path, expected: &PatchManifest) -> bool {
    let folder = runtime.join(RAAMS_SHADOW_PATCH);
    folder.join(PATCH_FILE).is_file()
        && fs::read_to_string(folder.join(PATCH_MANIFEST))
            .ok()
            .and_then(|text| serde_json::from_str::<PatchManifest>(&text).ok())
            .is_some_and(|manifest| &manifest == expected)
}

/// Runs the runtime for a one-off task with `arguments` and reads the JSON line it prints.
fn run_tool(runtime: &Path, arguments: &[String], timeout: Duration) -> Result<ToolReport, String> {
    let executable = crate::runtime::executable_path(runtime);
    if !executable.is_file() {
        return Err("the runtime is not installed".into());
    }
    let mut child = crate::host_env::use_host_env(&mut Command::new(&executable))
        .args(arguments)
        .current_dir(runtime)
        // As for the achievements export: no Dock icon on macOS for this background task.
        .env(crate::achievements::MAC_BACKGROUND_APP_HINT, "1")
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .map_err(|e| format!("cannot run {}: {e}", executable.display()))?;
    let deadline = Instant::now() + timeout;
    loop {
        if child.try_wait().map_err(|e| e.to_string())?.is_some() {
            break;
        }
        if Instant::now() >= deadline {
            let _ = child.kill();
            let _ = child.wait();
            return Err("the runtime did not finish".into());
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    let mut stdout = String::new();
    if let Some(mut pipe) = child.stdout.take() {
        let _ = pipe.read_to_string(&mut stdout);
    }
    let line = stdout.lines().rev().find(|line| line.starts_with('{')).unwrap_or("");
    serde_json::from_str(line).map_err(|_| "the runtime gave no answer".to_string())
}

/// Makes RAAM's Shadow's patch for `game` in the runtime folder, unless the one there is already
/// for these game files and this runtime.
pub fn prepare_patch(runtime: &Path, game: &Path) -> Result<(), String> {
    let expected = expected_manifest(runtime, game).ok_or("the game files or the runtime cannot be read")?;
    if patch_ready(runtime, &expected) {
        return Ok(());
    }
    let folder = runtime.join(RAAMS_SHADOW_PATCH);
    let _ = fs::remove_dir_all(&folder);
    let report = run_tool(
        runtime,
        &[format!("--game_data_root={}", game.display()), format!("--make_dlc_patch={}", folder.display())],
        PATCH_TIMEOUT,
    )?;
    if !report.ok {
        return Err(report.error);
    }
    let manifest = serde_json::to_string_pretty(&expected).map_err(|e| e.to_string())?;
    fs::write(folder.join(PATCH_MANIFEST), manifest).map_err(|e| format!("cannot write the patch manifest: {e}"))
}

/// The downloadable content state, for the interface.
///
/// Makes RAAM's Shadow's patch when the add-on is installed and the patch is missing or was made for
/// other game files or another runtime, unless the runtime is not `runtime_current` (an older one
/// does not know how to, and would start the game instead).
pub fn state(runtime: &Path, game: Option<&Path>, saves: &Path, runtime_current: bool) -> DlcState {
    let packages = installed_packages(saves);
    let raams_package = raams_shadow(saves);
    let raams_status = match (&raams_package, game) {
        (None, _) => DlcStatus::NotInstalled,
        (Some(_), _) if !runtime_current => DlcStatus::NeedsRuntime,
        (Some(_), Some(game)) if prepare_patch(runtime, game).is_ok() => DlcStatus::Ready,
        (Some(_), _) => DlcStatus::Unsupported,
    };
    let entries = CATALOG
        .iter()
        .map(|&id| DlcEntry { id, status: if id == "raamsShadow" { raams_status } else { DlcStatus::Multiplayer } })
        .collect();
    DlcState {
        entries,
        other_packages: packages.iter().filter(|p| Some(*p) != raams_package.as_ref()).count(),
        packages: packages.iter().filter_map(|p| p.file_name()).map(|n| n.to_string_lossy().into_owned()).collect(),
        raams_shadow: raams_package.is_some(),
        raams_shadow_playable: raams_status == DlcStatus::Ready,
    }
}

/// Arguments that start RAAM's Shadow's support when it can be played: the patch as the update
/// volume and the campaign menu's DLC option. None otherwise. Makes the patch first when needed.
pub fn launch_arguments(runtime: &Path, game: Option<&Path>, saves: &Path) -> Vec<String> {
    let Some(game) = game else {
        return Vec::new();
    };
    if raams_shadow(saves).is_none() || prepare_patch(runtime, game).is_err() {
        return Vec::new();
    }
    vec![
        format!("--update_data_root={}", runtime.join(RAAMS_SHADOW_PATCH).display()),
        "--dlc_campaign=true".into(),
    ]
}

/// The first bytes of an Xbox 360 content package: LIVE and PIRS for Microsoft's, CON for a
/// console's own.
const PACKAGE_MAGICS: [&[u8; 4]; 3] = [b"LIVE", b"PIRS", b"CON "];

/// Where a package shared as a .zip is extracted for the installation, in the saves folder (the
/// same disk the installed content goes to), removed afterwards.
const ZIP_EXTRACT_DIR: &str = ".dlc-from-zip";

fn is_zip(path: &Path) -> bool {
    let mut magic = [0u8; 4];
    fs::File::open(path).and_then(|mut file| file.read_exact(&mut magic)).is_ok() && &magic == b"PK\x03\x04"
}

/// Extracts the first Xbox 360 content package inside `archive` into `folder`, under its own file
/// name (the installed content keeps it), and returns its path.
fn extract_package_from_zip(archive: &Path, folder: &Path) -> Result<PathBuf, String> {
    let file = fs::File::open(archive).map_err(|e| format!("cannot open {}: {e}", archive.display()))?;
    let mut zip = zip::ZipArchive::new(file).map_err(|e| format!("the .zip cannot be read: {e}"))?;
    for index in 0..zip.len() {
        let mut entry = zip.by_index(index).map_err(|e| format!("the .zip cannot be read: {e}"))?;
        if entry.is_dir() {
            continue;
        }
        let mut magic = [0u8; 4];
        if entry.read_exact(&mut magic).is_err() || !PACKAGE_MAGICS.contains(&&magic) {
            continue;
        }
        let name = Path::new(entry.name()).file_name().ok_or("a package in the .zip has no name")?.to_owned();
        fs::create_dir_all(folder).map_err(|e| format!("cannot create {}: {e}", folder.display()))?;
        let path = folder.join(name);
        let mut out = fs::File::create(&path).map_err(|e| format!("cannot write {}: {e}", path.display()))?;
        out.write_all(&magic)
            .and_then(|()| io::copy(&mut entry, &mut out).map(|_| ()))
            .map_err(|e| format!("cannot extract the package from the .zip: {e}"))?;
        return Ok(path);
    }
    Err("the .zip has no Xbox 360 content package".into())
}

/// Installs `package` into the saves folder with the runtime, and makes RAAM's Shadow's patch when
/// the package is that add-on. Returns the package's name. A .zip holding the package (as packages
/// are often shared) is extracted first.
pub fn install(runtime: &Path, game: &Path, saves: &Path, package: &Path) -> Result<String, String> {
    if !is_zip(package) {
        return install_package(runtime, game, saves, package);
    }
    let folder = saves.join(ZIP_EXTRACT_DIR);
    let _ = fs::remove_dir_all(&folder);
    let result = extract_package_from_zip(package, &folder)
        .and_then(|extracted| install_package(runtime, game, saves, &extracted));
    let _ = fs::remove_dir_all(&folder);
    result
}

fn install_package(runtime: &Path, game: &Path, saves: &Path, package: &Path) -> Result<String, String> {
    let report = run_tool(
        runtime,
        &[
            format!("--game_data_root={}", game.display()),
            format!("--user_data_root={}", saves.display()),
            format!("--install_content={}", package.display()),
        ],
        INSTALL_TIMEOUT,
    )
    .map_err(|e| if e == "the runtime did not finish" { "the runtime did not finish the installation".into() } else { e })?;
    if !report.ok {
        return Err(report.error);
    }
    if raams_shadow(saves).is_some() {
        prepare_patch(runtime, game)?;
    }
    Ok(report.display_name)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("azura-dlc-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn finds_raams_shadow_by_its_first_level() {
        let saves = temp("find");
        let other = content_dir(&saves).join("AAAA");
        fs::create_dir_all(&other).unwrap();
        assert!(raams_shadow(&saves).is_none());
        let package = content_dir(&saves).join("BBBB");
        fs::create_dir_all(package.join("GearGame/CookedXbox360")).unwrap();
        fs::write(package.join(RAAMS_SHADOW_MARKER), b"").unwrap();
        assert_eq!(raams_shadow(&saves), Some(package));
        let _ = fs::remove_dir_all(&saves);
    }

    #[test]
    fn extracts_the_package_from_a_zip() {
        let root = temp("zip");
        let archive = root.join("pack.zip");
        let mut writer = zip::ZipWriter::new(fs::File::create(&archive).unwrap());
        let options =
            zip::write::SimpleFileOptions::default().compression_method(zip::CompressionMethod::Deflated);
        writer.start_file("readme.txt", options).unwrap();
        writer.write_all(b"not a package").unwrap();
        writer.start_file("4D5308AB/00000002/ABCD", options).unwrap();
        writer.write_all(b"LIVE and the rest of the package").unwrap();
        writer.finish().unwrap();
        assert!(is_zip(&archive));
        let extracted = extract_package_from_zip(&archive, &root.join("out")).unwrap();
        assert_eq!(extracted.file_name().unwrap(), "ABCD");
        assert_eq!(fs::read(&extracted).unwrap(), b"LIVE and the rest of the package");
        let empty = root.join("empty.zip");
        let mut writer = zip::ZipWriter::new(fs::File::create(&empty).unwrap());
        writer.start_file("readme.txt", options).unwrap();
        writer.write_all(b"not a package").unwrap();
        writer.finish().unwrap();
        assert!(extract_package_from_zip(&empty, &root.join("out2")).is_err());
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn identifies_a_disc_image_by_its_size_and_date() {
        let root = temp("disc");
        let image = root.join("game.iso");
        fs::write(&image, b"an image of the disc").unwrap();
        let identity = game_files_identity(&image).unwrap();
        assert!(identity.starts_with("disc image 20:"), "{identity}");
        // A folder is identified by its GearGame.xxx, and has none here.
        assert!(game_files_identity(&root).is_none());
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn starts_the_patch_only_when_it_was_made_for_these_game_files_and_runtime() {
        let root = temp("args");
        let (runtime, game, saves) = (root.join("runtime"), root.join("game"), root.join("saves"));
        let package = content_dir(&saves).join("P");
        fs::create_dir_all(package.join("GearGame/CookedXbox360")).unwrap();
        fs::write(package.join(RAAMS_SHADOW_MARKER), b"").unwrap();
        fs::create_dir_all(game.join("GearGame/CookedXbox360")).unwrap();
        fs::write(game.join("GearGame/CookedXbox360/GearGame.xxx"), b"disc").unwrap();
        // A runtime that cannot run: the patch can only be the one already made.
        fs::create_dir_all(&runtime).unwrap();
        fs::write(crate::runtime::executable_path(&runtime), b"not a program").unwrap();
        let folder = runtime.join(RAAMS_SHADOW_PATCH);
        fs::create_dir_all(folder.join("GearGame/Patches")).unwrap();
        fs::write(folder.join(PATCH_FILE), b"patch").unwrap();
        let expected = expected_manifest(&runtime, &game).unwrap();
        let other = PatchManifest { gear_game: "0000".into(), runtime: expected.runtime.clone() };
        fs::write(folder.join(PATCH_MANIFEST), serde_json::to_string(&other).unwrap()).unwrap();
        assert!(launch_arguments(&runtime, Some(&game), &saves).is_empty());
        fs::create_dir_all(folder.join("GearGame/Patches")).unwrap();
        fs::write(folder.join(PATCH_FILE), b"patch").unwrap();
        fs::write(folder.join(PATCH_MANIFEST), serde_json::to_string(&expected).unwrap()).unwrap();
        let args = launch_arguments(&runtime, Some(&game), &saves);
        assert_eq!(args.len(), 2);
        assert!(args[0].starts_with("--update_data_root="));
        assert_eq!(args[1], "--dlc_campaign=true");
        let listed = state(&runtime, Some(&game), &saves, true);
        assert!(listed.raams_shadow_playable);
        assert_eq!(listed.entries[0], DlcEntry { id: "raamsShadow", status: DlcStatus::Ready });
        assert!(listed.entries[1..].iter().all(|entry| entry.status == DlcStatus::Multiplayer));
        assert_eq!(listed.other_packages, 0);
        // An older runtime is not asked to make the patch.
        assert_eq!(state(&runtime, Some(&game), &saves, false).entries[0].status, DlcStatus::NeedsRuntime);
        let _ = fs::remove_dir_all(&root);
    }
}
