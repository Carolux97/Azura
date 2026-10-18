//! Reads and writes the launcher's settings.
//!
//! They live in two files under the app data folder
//! (`~/Library/Application Support/com.gears3azura.launcher/` on macOS):
//!
//! - `runtime/azura.toml`: the runtime's own config file, which the runtime reads from beside its
//!   executable. It holds every setting that changes how the game runs, under the runtime's
//!   setting (cvar) names. The in-game settings menu (F4) writes to it too, so the launcher only
//!   ever rewrites its own keys and keeps everything else, comments included.
//! - `launcher.toml`: what only the launcher uses (interface language, chosen preset, which game
//!   language was picked, and the saves folder, which the runtime receives on the command line;
//!   see `saves.rs`). The game language also reaches the runtime as `user_language`.
//!
//! Graphics values are passed through as found: a key missing from `azura.toml` is sent as
//! `null`, and the interface fills it from the selected preset (the preset table lives in
//! `launcher/src/graphics.js`, so it is not duplicated here).

use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};
use toml_edit::{value, DocumentMut, Item};

/// Runtime config file, relative to the app data folder.
const RUNTIME_CONFIG: &str = "runtime/azura.toml";

/// Launcher-only config file, relative to the app data folder.
const LAUNCHER_CONFIG: &str = "launcher.toml";

/// Header written at the top of a new `azura.toml`.
const RUNTIME_CONFIG_HEADER: &str = "\
# Gears3-Azura runtime settings.
#
# Written by the launcher and by the in-game settings menu (F4). Every key is a runtime setting
# (cvar) name; azura.default.toml, beside the runtime, documents all of them.
";

// Runtime setting names the launcher writes. They are defined by the ReXGlue SDK.
const KEY_GAME_DATA_ROOT: &str = "game_data_root";
const KEY_USER_NAME: &str = "user_name";
const KEY_USER_LANGUAGE: &str = "user_language";
const KEY_USER_COUNTRY: &str = "user_country";
const KEY_GPU_PLUGIN: &str = "gpu_plugin";
const KEY_FULLSCREEN: &str = "fullscreen";
const KEY_RESOLUTION: &str = "resolution";
const KEY_WINDOW_WIDTH: &str = "window_width";
const KEY_WINDOW_HEIGHT: &str = "window_height";
const KEY_SCALE_X: &str = "draw_resolution_scale_x";
const KEY_SCALE_Y: &str = "draw_resolution_scale_y";
const KEY_ANISOTROPIC: &str = "anisotropic_override";
const KEY_ANTIALIASING: &str = "swap_post_effect";
const KEY_UPSCALING: &str = "present_effect";
const KEY_TEXTURE_SOFT: &str = "texture_cache_memory_limit_soft";
const KEY_TEXTURE_HARD: &str = "texture_cache_memory_limit_hard";

/// Console language ID (the SDK's `XLanguage`) for an engine language code. Czech and Hungarian
/// have none on the console, so they are not passed to the runtime.
fn console_language(code: &str) -> Option<i64> {
    Some(match code {
        "INT" => 1,
        "JPN" => 2,
        "DEU" => 3,
        "FRA" => 4,
        "ESN" | "ESM" => 5,
        "ITA" => 6,
        "KOR" => 7,
        "CHN" => 8,
        "PTB" => 9,
        "POL" => 11,
        "RUS" => 12,
        _ => return None,
    })
}

fn console_country(code: &str) -> Option<i64> {
    Some(match code {
        "ESN" => 31,
        "ESM" => 43,
        "INT" => 103,
        "FRA" => 28,
        "DEU" => 29,
        "ITA" => 32,
        "KOR" => 40,
        "CHN" => 101,
        "PTB" => 10,
        "POL" => 49,
        "RUS" => 53,
        "CZE" => 21,
        "HUN" => 30,
        "JPN" => 36,
        _ => return None,
    })
}

/// GPU plugin the runtime needs to draw anything; written on every save.
const GPU_PLUGIN: &str = "xenos";

/// Allowed types for the resolution setting (integer scale 1, 2, 3 or string "1080p").
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(untagged)]
pub enum ResolutionScale {
    Int(i64),
    String(String),
}

impl From<i64> for ResolutionScale {
    fn from(val: i64) -> Self {
        ResolutionScale::Int(val)
    }
}

impl From<&str> for ResolutionScale {
    fn from(val: &str) -> Self {
        ResolutionScale::String(val.to_string())
    }
}

/// Graphics values as stored in `azura.toml`; `None` when the key is absent or has another type.
#[derive(Debug, Default, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct GraphicsValues {
    pub resolution_scale: Option<ResolutionScale>,
    pub anisotropic: Option<i64>,
    pub antialiasing: Option<String>,
    pub upscaling: Option<String>,
    pub texture_memory: Option<i64>,
}

/// Everything the interface reads and writes, exchanged with it as JSON.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Settings {
    /// Game folder (`game_data_root` in `azura.toml`).
    pub game_folder: Option<String>,
    /// Gamertag / player name (`user_name` in `azura.toml`).
    pub user_name: Option<String>,
    /// Display mode (`fullscreen` in `azura.toml`).
    pub fullscreen: bool,
    /// Graphics values found in `azura.toml`.
    pub graphics: GraphicsValues,
    /// Preset the user selected: "low", "medium", "high", "auto" or "custom".
    pub graphics_preset: String,
    /// Interface language: "auto" (follow the system) or a launcher locale code.
    pub ui_language: String,
    /// Engine language code of the game ("INT", "ESN", ...), when one is selected.
    pub game_language: Option<String>,
    /// Whether the user picked `game_language` (an automatic choice is recomputed per folder).
    pub game_language_chosen: bool,
    /// Folder that holds the saves. Read-only here: `save` keeps the stored one, and only moving
    /// the saves (`SettingsPaths::set_saves_folder`) changes it, so the interface may leave it out.
    #[serde(default)]
    pub saves_folder: String,
}

/// The part of [`Settings`] stored in `launcher.toml`. Every field has a default, so an old or
/// partial file still loads.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default)]
struct LauncherFile {
    graphics_preset: String,
    ui_language: String,
    game_language: Option<String>,
    game_language_chosen: bool,
    saves_folder: Option<String>,
}

impl Default for LauncherFile {
    fn default() -> Self {
        Self {
            graphics_preset: "auto".into(),
            ui_language: "auto".into(),
            game_language: None,
            game_language_chosen: false,
            saves_folder: None,
        }
    }
}

/// Reads a file that may not exist yet; a missing file reads as empty.
fn read_optional(path: &Path) -> io::Result<String> {
    match fs::read_to_string(path) {
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(String::new()),
        result => result,
    }
}

/// Writes a file by writing a sibling temporary file and renaming it over the target, so a crash
/// or a full disk never leaves a half-written config behind.
fn write_atomically(path: &Path, contents: &str) -> io::Result<()> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    let temporary = path.with_extension("toml.tmp");
    fs::write(&temporary, contents)?;
    fs::rename(&temporary, path)
}

fn get_string(document: &DocumentMut, key: &str) -> Option<String> {
    document.get(key)?.as_str().map(str::to_owned)
}

fn get_integer(document: &DocumentMut, key: &str) -> Option<i64> {
    document.get(key)?.as_integer()
}

/// Sets `key` to `item`, or removes it when `item` is `None` (a value the interface left unset).
fn set_or_remove(document: &mut DocumentMut, key: &str, item: Option<Item>) {
    match item {
        Some(item) => document[key] = item,
        None => {
            document.remove(key);
        }
    }
}

/// Paths of the two config files under an app data folder.
pub struct SettingsPaths {
    runtime_config: PathBuf,
    launcher_config: PathBuf,
}

impl SettingsPaths {
    pub fn new(app_data_dir: &Path) -> Self {
        Self {
            runtime_config: app_data_dir.join(RUNTIME_CONFIG),
            launcher_config: app_data_dir.join(LAUNCHER_CONFIG),
        }
    }

    fn load_launcher_file(&self) -> Result<LauncherFile, String> {
        let text = read_optional(&self.launcher_config).map_err(|e| e.to_string())?;
        Ok(toml::from_str(&text).unwrap_or_default())
    }

    fn save_launcher_file(&self, file: &LauncherFile) -> Result<(), String> {
        let text = toml::to_string(file).map_err(|e| e.to_string())?;
        write_atomically(&self.launcher_config, &text).map_err(|e| e.to_string())
    }

    /// Folder for the saves: the one chosen, or the default.
    pub fn saves_folder(&self) -> Result<PathBuf, String> {
        Ok(self
            .load_launcher_file()?
            .saves_folder
            .map(PathBuf::from)
            .unwrap_or_else(crate::saves::default_folder))
    }

    /// Records `folder` as the saves folder (after the saves have been moved there).
    pub fn set_saves_folder(&self, folder: &Path) -> Result<(), String> {
        let mut file = self.load_launcher_file()?;
        file.saves_folder = Some(folder.to_string_lossy().into_owned());
        self.save_launcher_file(&file)
    }

    /// Loads the settings; missing files and missing keys load as defaults.
    pub fn load(&self) -> Result<Settings, String> {
        let runtime_text = read_optional(&self.runtime_config).map_err(|e| e.to_string())?;
        let runtime: DocumentMut = runtime_text
            .parse()
            .map_err(|e| format!("{} is not valid TOML: {e}", self.runtime_config.display()))?;

        let launcher = self.load_launcher_file()?;
        let saves_folder = launcher
            .saves_folder
            .clone()
            .unwrap_or_else(|| crate::saves::default_folder().to_string_lossy().into_owned());

        let scale_int = get_integer(&runtime, KEY_SCALE_X);
        let res_str = get_string(&runtime, KEY_RESOLUTION);
        let win_w = get_integer(&runtime, KEY_WINDOW_WIDTH);

        let resolution_scale = if res_str.as_deref() == Some("1080p")
            || res_str.as_deref() == Some("1920x1080")
            || (win_w == Some(1920) && (scale_int == Some(1) || scale_int.is_none()))
        {
            Some(ResolutionScale::String("1080p".into()))
        } else {
            scale_int.map(ResolutionScale::Int)
        };

        Ok(Settings {
            game_folder: get_string(&runtime, KEY_GAME_DATA_ROOT),
            user_name: get_string(&runtime, KEY_USER_NAME),
            fullscreen: runtime
                .get(KEY_FULLSCREEN)
                .and_then(Item::as_bool)
                .unwrap_or(false),
            graphics: GraphicsValues {
                resolution_scale,
                anisotropic: get_integer(&runtime, KEY_ANISOTROPIC),
                antialiasing: get_string(&runtime, KEY_ANTIALIASING),
                upscaling: get_string(&runtime, KEY_UPSCALING),
                texture_memory: get_integer(&runtime, KEY_TEXTURE_SOFT),
            },
            graphics_preset: launcher.graphics_preset,
            ui_language: launcher.ui_language,
            game_language: launcher.game_language,
            game_language_chosen: launcher.game_language_chosen,
            saves_folder,
        })
    }

    /// Saves the settings. `azura.toml` is re-read first and only the launcher's keys change, so
    /// what the in-game menu wrote in the meantime is kept.
    pub fn save(&self, settings: &Settings) -> Result<(), String> {
        let runtime_text = read_optional(&self.runtime_config).map_err(|e| e.to_string())?;
        // A new file gets the header prepended on write: a document holding only comments keeps
        // them as trailing text, so parsing the header first would put it after the keys.
        let is_new = runtime_text.trim().is_empty();
        let mut runtime: DocumentMut = runtime_text
            .parse()
            .map_err(|e| format!("{} is not valid TOML: {e}", self.runtime_config.display()))?;

        let graphics = &settings.graphics;
        runtime[KEY_GPU_PLUGIN] = value(GPU_PLUGIN);
        runtime[KEY_FULLSCREEN] = value(settings.fullscreen);
        set_or_remove(&mut runtime, KEY_GAME_DATA_ROOT, settings.game_folder.as_deref().map(value));
        set_or_remove(
            &mut runtime,
            KEY_USER_NAME,
            settings
                .user_name
                .as_deref()
                .map(str::trim)
                .filter(|s| !s.is_empty())
                .map(value),
        );
        set_or_remove(
            &mut runtime,
            KEY_USER_LANGUAGE,
            settings.game_language.as_deref().and_then(console_language).map(value),
        );
        set_or_remove(
            &mut runtime,
            KEY_USER_COUNTRY,
            settings.game_language.as_deref().and_then(console_country).map(value),
        );

        match &graphics.resolution_scale {
            Some(ResolutionScale::String(s)) if s == "1080p" || s == "1920x1080" => {
                runtime[KEY_RESOLUTION] = value("1080p");
                runtime[KEY_WINDOW_WIDTH] = value(1920);
                runtime[KEY_WINDOW_HEIGHT] = value(1080);
                runtime[KEY_SCALE_X] = value(1);
                runtime[KEY_SCALE_Y] = value(1);
            }
            Some(ResolutionScale::Int(1)) => {
                runtime[KEY_RESOLUTION] = value("720p");
                runtime[KEY_WINDOW_WIDTH] = value(1280);
                runtime[KEY_WINDOW_HEIGHT] = value(720);
                runtime[KEY_SCALE_X] = value(1);
                runtime[KEY_SCALE_Y] = value(1);
            }
            Some(ResolutionScale::Int(2)) => {
                runtime[KEY_RESOLUTION] = value("1440p");
                runtime[KEY_WINDOW_WIDTH] = value(2560);
                runtime[KEY_WINDOW_HEIGHT] = value(1440);
                runtime[KEY_SCALE_X] = value(2);
                runtime[KEY_SCALE_Y] = value(2);
            }
            Some(ResolutionScale::Int(3)) => {
                runtime[KEY_RESOLUTION] = value("4k");
                runtime[KEY_WINDOW_WIDTH] = value(3840);
                runtime[KEY_WINDOW_HEIGHT] = value(2160);
                runtime[KEY_SCALE_X] = value(3);
                runtime[KEY_SCALE_Y] = value(3);
            }
            None => {
                runtime.remove(KEY_RESOLUTION);
                runtime.remove(KEY_WINDOW_WIDTH);
                runtime.remove(KEY_WINDOW_HEIGHT);
                runtime.remove(KEY_SCALE_X);
                runtime.remove(KEY_SCALE_Y);
            }
            Some(other) => {
                if let ResolutionScale::Int(scale) = other {
                    runtime[KEY_SCALE_X] = value(*scale);
                    runtime[KEY_SCALE_Y] = value(*scale);
                }
            }
        }
        set_or_remove(&mut runtime, KEY_ANISOTROPIC, graphics.anisotropic.map(value));
        set_or_remove(&mut runtime, KEY_ANTIALIASING, graphics.antialiasing.as_deref().map(value));
        set_or_remove(&mut runtime, KEY_UPSCALING, graphics.upscaling.as_deref().map(value));
        set_or_remove(&mut runtime, KEY_TEXTURE_SOFT, graphics.texture_memory.map(value));
        // The hard limit is kept at twice the soft one, as in the SDK defaults (384 / 768 MB).
        set_or_remove(&mut runtime, KEY_TEXTURE_HARD, graphics.texture_memory.map(|mb| value(mb * 2)));
        let runtime_out = if is_new {
            format!("{RUNTIME_CONFIG_HEADER}\n{runtime}")
        } else {
            runtime.to_string()
        };
        write_atomically(&self.runtime_config, &runtime_out).map_err(|e| e.to_string())?;

        let launcher = LauncherFile {
            graphics_preset: settings.graphics_preset.clone(),
            ui_language: settings.ui_language.clone(),
            game_language: settings.game_language.clone(),
            game_language_chosen: settings.game_language_chosen,
            saves_folder: self.load_launcher_file()?.saves_folder,
        };
        self.save_launcher_file(&launcher)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_support::TempFolder;

    /// Creates an empty folder for one test, removed when the test ends.
    fn temp_folder(name: &str) -> TempFolder {
        TempFolder::new(name)
    }

    fn sample_settings() -> Settings {
        Settings {
            game_folder: Some("/Games/Gears of War 3".into()),
            user_name: Some("Marcus".into()),
            fullscreen: true,
            graphics: GraphicsValues {
                resolution_scale: Some(2.into()),
                anisotropic: Some(5),
                antialiasing: Some("fxaa".into()),
                upscaling: Some("fsr".into()),
                texture_memory: Some(768),
            },
            graphics_preset: "high".into(),
            ui_language: "es".into(),
            game_language: Some("ESN".into()),
            game_language_chosen: true,
            saves_folder: crate::saves::default_folder().to_string_lossy().into_owned(),
        }
    }

    #[test]
    fn missing_files_load_as_defaults() {
        let dir = temp_folder("defaults");
        let settings = SettingsPaths::new(&dir).load().unwrap();
        assert_eq!(settings.game_folder, None);
        assert_eq!(settings.user_name, None);
        assert!(!settings.fullscreen);
        assert_eq!(settings.graphics, GraphicsValues::default());
        assert_eq!(settings.graphics_preset, "auto");
        assert_eq!(settings.ui_language, "auto");
    }

    #[test]
    fn the_saves_folder_changes_only_through_set_saves_folder() {
        let dir = temp_folder("saves-folder");
        let paths = SettingsPaths::new(&dir);
        assert_eq!(paths.saves_folder().unwrap(), crate::saves::default_folder());

        paths.set_saves_folder(Path::new("/Volumes/Backup/Gears")).unwrap();
        let mut settings = sample_settings();
        settings.saves_folder = "/somewhere/else".into();
        paths.save(&settings).unwrap();
        assert_eq!(paths.saves_folder().unwrap(), PathBuf::from("/Volumes/Backup/Gears"));
        assert_eq!(paths.load().unwrap().saves_folder, "/Volumes/Backup/Gears");
    }

    #[test]
    fn settings_without_a_saves_folder_still_deserialize() {
        // The interface may send its settings without the saves folder; they must still save.
        let json = r#"{"gameFolder":"/Games/Gears of War 3","userName":"Marcus","fullscreen":false,
            "graphics":{},"graphicsPreset":"auto","uiLanguage":"auto","gameLanguage":null,
            "gameLanguageChosen":false}"#;
        let settings: Settings = serde_json::from_str(json).unwrap();
        assert_eq!(settings.game_folder.as_deref(), Some("/Games/Gears of War 3"));
    }

    #[test]
    fn saved_settings_load_back_unchanged() {
        let dir = temp_folder("roundtrip");
        let paths = SettingsPaths::new(&dir);
        paths.save(&sample_settings()).unwrap();
        assert_eq!(paths.load().unwrap(), sample_settings());
    }

    #[test]
    fn writes_the_runtime_setting_names() {
        let dir = temp_folder("names");
        SettingsPaths::new(&dir).save(&sample_settings()).unwrap();
        let text = fs::read_to_string(dir.join(RUNTIME_CONFIG)).unwrap();
        for line in [
            "gpu_plugin = \"xenos\"",
            "user_name = \"Marcus\"",
            "fullscreen = true",
            "game_data_root = \"/Games/Gears of War 3\"",
            "user_language = 5",
            "draw_resolution_scale_x = 2",
            "draw_resolution_scale_y = 2",
            "anisotropic_override = 5",
            "swap_post_effect = \"fxaa\"",
            "present_effect = \"fsr\"",
            "texture_cache_memory_limit_soft = 768",
            "texture_cache_memory_limit_hard = 1536",
        ] {
            assert!(text.contains(line), "missing `{line}` in:\n{text}");
        }
    }

    #[test]
    fn a_new_runtime_config_starts_with_its_header() {
        let dir = temp_folder("header");
        SettingsPaths::new(&dir).save(&sample_settings()).unwrap();
        let text = fs::read_to_string(dir.join(RUNTIME_CONFIG)).unwrap();
        assert!(text.starts_with(RUNTIME_CONFIG_HEADER), "header not first in:\n{text}");
    }

    #[test]
    fn keeps_keys_and_comments_written_by_others() {
        let dir = temp_folder("merge");
        let config = dir.join(RUNTIME_CONFIG);
        fs::create_dir_all(config.parent().unwrap()).unwrap();
        fs::write(&config, "# Set from the F4 menu.\nvsync = false\nanisotropic_override = 1\n").unwrap();

        SettingsPaths::new(&dir).save(&sample_settings()).unwrap();
        let text = fs::read_to_string(&config).unwrap();
        assert!(text.contains("# Set from the F4 menu."));
        assert!(text.contains("vsync = false"));
        assert!(text.contains("anisotropic_override = 5"));
        assert!(!text.contains("anisotropic_override = 1"));
    }

    #[test]
    fn unset_values_remove_their_keys() {
        let dir = temp_folder("remove");
        let paths = SettingsPaths::new(&dir);
        paths.save(&sample_settings()).unwrap();
        let mut cleared = sample_settings();
        cleared.game_folder = None;
        cleared.user_name = None;
        paths.save(&cleared).unwrap();
        let text = fs::read_to_string(dir.join(RUNTIME_CONFIG)).unwrap();
        assert!(!text.contains("game_data_root"));
        assert!(!text.contains("user_name"));
    }

    #[test]
    fn values_of_the_wrong_type_load_as_missing() {
        let dir = temp_folder("types");
        let config = dir.join(RUNTIME_CONFIG);
        fs::create_dir_all(config.parent().unwrap()).unwrap();
        fs::write(&config, "anisotropic_override = \"high\"\nfullscreen = 1\n").unwrap();
        let settings = SettingsPaths::new(&dir).load().unwrap();
        assert_eq!(settings.graphics.anisotropic, None);
        assert!(!settings.fullscreen);
    }

    #[test]
    fn invalid_runtime_config_is_reported_not_overwritten() {
        let dir = temp_folder("invalid");
        let config = dir.join(RUNTIME_CONFIG);
        fs::create_dir_all(config.parent().unwrap()).unwrap();
        fs::write(&config, "this is = = not toml").unwrap();
        let paths = SettingsPaths::new(&dir);
        assert!(paths.load().is_err());
        assert!(paths.save(&sample_settings()).is_err());
        assert_eq!(fs::read_to_string(&config).unwrap(), "this is = = not toml");
    }
}
