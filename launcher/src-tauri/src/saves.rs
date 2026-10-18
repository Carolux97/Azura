//! Where the game keeps its saves, and moving them when the player picks another folder.
//!
//! The runtime stores everything it writes for the player (campaign checkpoints, player storage,
//! profiles) under its user data folder (`user_data_root`), laid out as on the console:
//! `<XUID>/<title ID>/...` for saved content and `<title ID>/profile/...` for profiles. The
//! launcher chooses that folder and passes it to the runtime on start-up.
//!
//! Before the launcher managed it, the runtime used `~/.local/share/azura`. Its saves are moved to
//! the chosen folder the first time the game starts from the launcher. The shader cache that
//! folder also holds is not a save: it goes to the launcher's own cache folder instead.

use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Manager};

/// Folder for saves when the player has not chosen one, relative to the home folder.
const DEFAULT_SAVES: &str = "Documents/Games Saves/Gears3-Azura";

/// Where the runtime kept its data before the launcher chose the folder, relative to home.
const LEGACY_USER_DATA: &str = ".local/share/azura";

/// Entry of the runtime's user data folder that is a cache, not a save.
const CACHE_ENTRY: &str = "cache";

/// Why saves could not be moved. Serialized as `{"kind": "...", "detail": "..."}`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(tag = "kind", content = "detail", rename_all = "camelCase")]
pub enum SavesError {
    /// The destination already holds saves; moving needs the player's permission to replace them.
    DestinationHasSaves(String),
    /// The destination is inside the folder being moved (or the other way round).
    NestedFolders,
    /// A file operation failed.
    Io(String),
}

fn home() -> PathBuf {
    std::env::var_os("HOME").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("/"))
}

/// Folder saves go to when the player has not chosen one.
pub fn default_folder() -> PathBuf {
    home().join(DEFAULT_SAVES)
}

/// Where the runtime kept its data before the launcher managed it.
pub fn legacy_folder() -> PathBuf {
    home().join(LEGACY_USER_DATA)
}

/// Entries of a user data folder that are saves (everything except the cache and hidden files).
fn save_entries(folder: &Path) -> Vec<PathBuf> {
    let Ok(read) = fs::read_dir(folder) else {
        return Vec::new();
    };
    read.filter_map(Result::ok)
        .filter(|entry| {
            let name = entry.file_name().to_string_lossy().into_owned();
            name != CACHE_ENTRY && !name.starts_with('.')
        })
        .map(|entry| entry.path())
        .collect()
}

/// Whether `folder` holds any saves.
pub fn has_saves(folder: &Path) -> bool {
    !save_entries(folder).is_empty()
}

/// Moves `from` to `to`, also across disks (where a rename is not possible) by copying and then
/// removing the original.
fn move_entry(from: &Path, to: &Path) -> io::Result<()> {
    if fs::rename(from, to).is_ok() {
        return Ok(());
    }
    copy_entry(from, to)?;
    if fs::symlink_metadata(from)?.is_dir() {
        fs::remove_dir_all(from)
    } else {
        fs::remove_file(from)
    }
}

fn copy_entry(from: &Path, to: &Path) -> io::Result<()> {
    if fs::symlink_metadata(from)?.is_dir() {
        fs::create_dir_all(to)?;
        for entry in fs::read_dir(from)? {
            let entry = entry?;
            copy_entry(&entry.path(), &to.join(entry.file_name()))?;
        }
        Ok(())
    } else {
        fs::copy(from, to).map(|_| ())
    }
}

fn remove_entry(path: &Path) -> io::Result<()> {
    match fs::symlink_metadata(path) {
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
        Err(error) => Err(error),
        Ok(meta) if meta.is_dir() => fs::remove_dir_all(path),
        Ok(_) => fs::remove_file(path),
    }
}

/// `path` with links resolved, also when it does not exist yet: its nearest existing ancestor is
/// resolved and the rest appended (on macOS `/tmp` is a link to `/private/tmp`, so comparing an
/// unresolved path with a resolved one would miss that one folder is inside the other).
fn canonical(path: &Path) -> PathBuf {
    let mut missing = Vec::new();
    let mut current = path.to_path_buf();
    loop {
        if let Ok(resolved) = fs::canonicalize(&current) {
            return missing.iter().rev().fold(resolved, |acc, part| acc.join(part));
        }
        match (current.file_name().map(|n| n.to_os_string()), current.parent()) {
            (Some(name), Some(parent)) => {
                missing.push(name);
                current = parent.to_path_buf();
            }
            _ => return path.to_path_buf(),
        }
    }
}

/// Moves the saves of `from` into `to`. When `to` already holds saves it stops with
/// `DestinationHasSaves` unless `replace` is set, in which case entries with the same name in `to`
/// are replaced. Moving a folder into itself does nothing.
pub fn move_saves(from: &Path, to: &Path, replace: bool) -> Result<usize, SavesError> {
    let (from_c, to_c) = (canonical(from), canonical(to));
    if from_c == to_c {
        return Ok(0);
    }
    if to_c.starts_with(&from_c) || from_c.starts_with(&to_c) {
        return Err(SavesError::NestedFolders);
    }
    let entries = save_entries(from);
    if entries.is_empty() {
        return Ok(0);
    }
    if has_saves(to) && !replace {
        return Err(SavesError::DestinationHasSaves(to.to_string_lossy().into_owned()));
    }
    fs::create_dir_all(to).map_err(|e| SavesError::Io(format!("{}: {e}", to.display())))?;
    for entry in &entries {
        let target = to.join(entry.file_name().unwrap_or_default());
        remove_entry(&target).map_err(|e| SavesError::Io(format!("{}: {e}", target.display())))?;
        move_entry(entry, &target).map_err(|e| SavesError::Io(format!("{}: {e}", entry.display())))?;
    }
    Ok(entries.len())
}

/// Gets `saves` ready before the game starts: creates it and, the first time, brings over the
/// saves the runtime wrote to its old folder and moves that folder's shader cache to `cache`.
/// Nothing is replaced: when both folders hold saves, the old ones stay where they are.
pub fn prepare(saves: &Path, cache: &Path) -> Result<(), SavesError> {
    let legacy = legacy_folder();
    if has_saves(&legacy) && !has_saves(saves) {
        move_saves(&legacy, saves, false)?;
    }
    let legacy_cache = legacy.join(CACHE_ENTRY);
    if legacy_cache.is_dir() && !cache.exists() {
        if let Some(parent) = cache.parent() {
            fs::create_dir_all(parent).map_err(|e| SavesError::Io(e.to_string()))?;
        }
        move_entry(&legacy_cache, cache).map_err(|e| SavesError::Io(e.to_string()))?;
    }
    fs::create_dir_all(saves).map_err(|e| SavesError::Io(format!("{}: {e}", saves.display())))?;
    fs::create_dir_all(cache).map_err(|e| SavesError::Io(format!("{}: {e}", cache.display())))
}

/// Information about a single save backup.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BackupInfo {
    pub id: String,
    pub date: String,
    pub size_bytes: u64,
}

/// Status of the game's save data.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SaveStatus {
    pub has_saves: bool,
    pub checkpoint_count: usize,
    pub is_corrupted: bool,
    pub status: String,
    pub message: String,
    pub save_path: String,
    pub last_saved: Option<String>,
    pub backups: Vec<BackupInfo>,
}

/// Resolves where Gears of War 3 content packages and headers are kept inside `saves_dir`.
pub fn find_gears3_content_dir(saves_dir: &Path) -> PathBuf {
    let p1 = saves_dir.join("B13EBABEBABEBABE").join("4D5308AB");
    if p1.exists() {
        return p1;
    }
    let p2 = saves_dir.join("4D5308AB");
    if p2.exists() {
        return p2;
    }
    p1
}

fn backups_dir(app: &AppHandle) -> Result<PathBuf, String> {
    let dir = app
        .path()
        .app_data_dir()
        .map_err(|e| e.to_string())?
        .join("backups");
    let _ = fs::create_dir_all(&dir);
    Ok(dir)
}

fn dir_size(path: &Path) -> u64 {
    let mut total = 0;
    if let Ok(entries) = fs::read_dir(path) {
        for entry in entries.flatten() {
            if let Ok(meta) = entry.metadata() {
                if meta.is_dir() {
                    total += dir_size(&entry.path());
                } else {
                    total += meta.len();
                }
            }
        }
    }
    total
}

/// Inspects the current state of saved games.
pub fn inspect_saves(app: &AppHandle, saves_dir: &Path) -> SaveStatus {
    let content_dir = find_gears3_content_dir(saves_dir);
    let mut has_saves = false;
    let mut checkpoint_count = 0;
    let mut is_corrupted = false;
    let mut latest_time: Option<std::time::SystemTime> = None;

    let headers_dir = content_dir.join("Headers").join("00000001");
    let packages_dir = content_dir.join("00000001");

    if headers_dir.is_dir() {
        if let Ok(entries) = fs::read_dir(&headers_dir) {
            for entry in entries.flatten() {
                let path = entry.path();
                if path.is_file() {
                    has_saves = true;
                    if let Ok(meta) = path.metadata() {
                        if let Ok(mtime) = meta.modified() {
                            latest_time = match latest_time {
                                Some(prev) => Some(std::cmp::max(prev, mtime)),
                                None => Some(mtime),
                            };
                        }
                    }
                    let stem = path.file_stem().and_then(|s| s.to_str()).unwrap_or("");
                    let pkg_dir = packages_dir.join(stem);
                    if !pkg_dir.exists() {
                        is_corrupted = true;
                    }
                }
            }
        }
    }

    if packages_dir.is_dir() {
        if let Ok(entries) = fs::read_dir(&packages_dir) {
            for entry in entries.flatten() {
                let path = entry.path();
                if path.is_dir() {
                    has_saves = true;
                    let mut valid_package = false;
                    if let Ok(pkg_entries) = fs::read_dir(&path) {
                        let count = pkg_entries
                            .flatten()
                            .filter(|e| e.metadata().map(|m| m.len() > 0).unwrap_or(false))
                            .count();
                        if count > 0 {
                            valid_package = true;
                            checkpoint_count += count;
                        }
                    }
                    if !valid_package {
                        is_corrupted = true;
                    }
                }
            }
        }
    }

    let mut last_saved = None;
    if let Some(time) = latest_time {
        if let Ok(elapsed) = time.duration_since(std::time::UNIX_EPOCH) {
            let secs = elapsed.as_secs();
            let hours = (secs / 3600) % 24;
            let minutes = (secs / 60) % 60;
            last_saved = Some(format!("{:02}:{:02} UTC", hours, minutes));
        }
    }

    let backups = list_backups_internal(app);

    let (status, message) = if is_corrupted {
        (
            "corrupted".to_string(),
            "Se detectaron cabeceras huérfanas o incompletas (causa del aviso de datos corruptos).".to_string(),
        )
    } else if has_saves && checkpoint_count > 0 {
        (
            "ok".to_string(),
            format!("Partidas guardadas intactas ({} archivos de punto de control).", checkpoint_count),
        )
    } else {
        (
            "empty".to_string(),
            "No hay partidas guardadas activas (listo para iniciar nueva partida limpia).".to_string(),
        )
    };

    SaveStatus {
        has_saves,
        checkpoint_count,
        is_corrupted,
        status,
        message,
        save_path: content_dir.to_string_lossy().to_string(),
        last_saved,
        backups,
    }
}

/// Headers in `headers_dir` whose content package in `packages_dir` is missing or empty, each with
/// the package folder (which may not exist).
fn orphaned_headers(headers_dir: &Path, packages_dir: &Path) -> Vec<(PathBuf, PathBuf)> {
    let mut orphans = Vec::new();
    let Ok(entries) = fs::read_dir(headers_dir) else {
        return orphans;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        let file_stem = path.file_stem().and_then(|s| s.to_str()).unwrap_or("");
        let pkg_dir = packages_dir.join(file_stem);
        let is_orphan = if !pkg_dir.exists() {
            true
        } else if pkg_dir.is_dir() {
            fs::read_dir(&pkg_dir).map(|e| e.count()).unwrap_or(0) == 0
        } else {
            false
        };
        if is_orphan {
            orphans.push((path, pkg_dir));
        }
    }
    orphans
}

/// Removes orphaned headers and their empty package folders. Only runs when the player asks for
/// it, and backs the saves up first: nothing is deleted if the backup fails.
pub fn repair_saves_internal(app: &AppHandle, saves_dir: &Path) -> Result<String, String> {
    let content_dir = find_gears3_content_dir(saves_dir);
    let headers_dir = content_dir.join("Headers").join("00000001");
    let packages_dir = content_dir.join("00000001");
    let orphans = orphaned_headers(&headers_dir, &packages_dir);
    if !orphans.is_empty() {
        create_backup_internal(app, saves_dir, Some("auto_before_repair".into()))?;
    }
    let mut removed_count = 0;
    for (header, pkg_dir) in orphans {
        let _ = fs::remove_file(&header);
        if pkg_dir.exists() {
            let _ = fs::remove_dir_all(&pkg_dir);
        }
        removed_count += 1;
    }

    Ok(format!(
        "Reparación completada. Se eliminaron {} elementos corruptos o huérfanos.",
        removed_count
    ))
}

/// Performs a complete reset of save data, backing up first.
pub fn reset_saves_internal(app: &AppHandle, saves_dir: &Path) -> Result<String, String> {
    // Nothing is deleted if the backup fails.
    create_backup_internal(app, saves_dir, Some("auto_before_reset".into()))?;

    let content_dir = find_gears3_content_dir(saves_dir);
    let headers_dir = content_dir.join("Headers");
    let packages_dir = content_dir.join("00000001");

    if headers_dir.exists() {
        let _ = fs::remove_dir_all(&headers_dir);
    }
    if packages_dir.exists() {
        let _ = fs::remove_dir_all(&packages_dir);
    }

    let _ = fs::create_dir_all(content_dir.join("Headers").join("00000001"));
    let _ = fs::create_dir_all(content_dir.join("00000001"));

    Ok("Guardado restablecido con éxito. Se ha creado una copia de seguridad automática.".into())
}

/// Creates a backup of current saves.
pub fn create_backup_internal(app: &AppHandle, saves_dir: &Path, label: Option<String>) -> Result<BackupInfo, String> {
    let content_dir = find_gears3_content_dir(saves_dir);
    let b_dir = backups_dir(app)?;

    let timestamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);

    let tag = label.unwrap_or_else(|| "manual".into());
    let backup_id = format!("backup_{}_{}", timestamp, tag);
    let target_dir = b_dir.join(&backup_id);

    if content_dir.exists() {
        copy_entry(&content_dir, &target_dir).map_err(|e| e.to_string())?;
    } else {
        fs::create_dir_all(&target_dir).map_err(|e| e.to_string())?;
    }

    let size_bytes = dir_size(&target_dir);

    Ok(BackupInfo {
        id: backup_id,
        date: format!("{}", timestamp),
        size_bytes,
    })
}

/// Lists available backups.
pub fn list_backups_internal(app: &AppHandle) -> Vec<BackupInfo> {
    let mut result = Vec::new();
    if let Ok(b_dir) = backups_dir(app) {
        if let Ok(entries) = fs::read_dir(&b_dir) {
            for entry in entries.flatten() {
                if entry.file_type().map(|t| t.is_dir()).unwrap_or(false) {
                    let id = entry.file_name().to_string_lossy().to_string();
                    let size_bytes = dir_size(&entry.path());
                    result.push(BackupInfo {
                        id,
                        date: "Copia disponible".into(),
                        size_bytes,
                    });
                }
            }
        }
    }
    result.sort_by(|a, b| b.id.cmp(&a.id));
    result
}

/// Restores a backup.
pub fn restore_backup_internal(app: &AppHandle, saves_dir: &Path, backup_id: &str) -> Result<String, String> {
    let b_dir = backups_dir(app)?;
    let src = b_dir.join(backup_id);
    if !src.is_dir() {
        return Err(format!("No existe la copia de seguridad: {}", backup_id));
    }

    // The backup is copied beside the current saves first, and only swapped in once the copy is
    // complete, so a failed copy leaves the current saves as they were.
    let content_dir = find_gears3_content_dir(saves_dir);
    let restoring = content_dir.with_extension("restoring");
    let previous = content_dir.with_extension("previous");
    let _ = fs::remove_dir_all(&restoring);
    let _ = fs::remove_dir_all(&previous);
    if let Err(error) = copy_entry(&src, &restoring) {
        let _ = fs::remove_dir_all(&restoring);
        return Err(error.to_string());
    }
    if content_dir.exists() {
        fs::rename(&content_dir, &previous).map_err(|e| e.to_string())?;
    }
    if let Err(error) = fs::rename(&restoring, &content_dir) {
        let _ = fs::rename(&previous, &content_dir);
        return Err(error.to_string());
    }
    let _ = fs::remove_dir_all(&previous);
    Ok("Partida restaurada con éxito.".into())
}

/// Opens the saves directory in the system file manager.
pub fn open_saves_folder_internal(saves_dir: &Path) -> Result<(), String> {
    let dir = find_gears3_content_dir(saves_dir);
    let _ = fs::create_dir_all(&dir);

    #[cfg(target_os = "linux")]
    {
        std::process::Command::new("xdg-open")
            .arg(&dir)
            .spawn()
            .map_err(|e| e.to_string())?;
    }

    #[cfg(target_os = "macos")]
    {
        std::process::Command::new("open")
            .arg(&dir)
            .spawn()
            .map_err(|e| e.to_string())?;
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_support::TempFolder;

    /// A user data folder as the runtime writes it: a checkpoint, a profile and a shader cache.
    fn fake_user_data(dir: &Path) {
        fs::create_dir_all(dir.join("B13EBABEBABEBABE/4D5308AB/00000001/GearsCheckpoint")).unwrap();
        fs::write(dir.join("B13EBABEBABEBABE/4D5308AB/00000001/PlayerStorage.dat"), b"save").unwrap();
        fs::create_dir_all(dir.join("4D5308AB/profile/Marcus")).unwrap();
        fs::create_dir_all(dir.join("cache/shaders")).unwrap();
    }

    #[test]
    fn moves_saves_but_not_the_cache() {
        let from = TempFolder::new("saves-from");
        let to = TempFolder::new("saves-to");
        fake_user_data(&from);

        assert_eq!(move_saves(&from, &to.join("Saves"), false).unwrap(), 2);
        assert!(to.join("Saves/B13EBABEBABEBABE/4D5308AB/00000001/PlayerStorage.dat").is_file());
        assert!(to.join("Saves/4D5308AB/profile/Marcus").is_dir());
        assert!(!to.join("Saves/cache").exists());
        assert!(from.join("cache/shaders").is_dir());
        assert!(!has_saves(&from));
    }

    #[test]
    fn asks_before_replacing_saves() {
        let from = TempFolder::new("replace-from");
        let to = TempFolder::new("replace-to");
        fake_user_data(&from);
        fs::create_dir_all(to.join("4D5308AB")).unwrap();

        assert!(matches!(move_saves(&from, &to, false), Err(SavesError::DestinationHasSaves(_))));
        assert!(has_saves(&from));
        assert_eq!(move_saves(&from, &to, true).unwrap(), 2);
        assert!(to.join("4D5308AB/profile/Marcus").is_dir());
    }

    #[test]
    fn refuses_a_folder_inside_the_other() {
        let from = TempFolder::new("saves-nested");
        fake_user_data(&from);
        assert_eq!(move_saves(&from, &from.join("inner"), false), Err(SavesError::NestedFolders));
        assert_eq!(move_saves(&from, &from, false).unwrap(), 0);
    }
}
