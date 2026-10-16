//! Reads and writes the launcher's settings.
//!
//! They live in two files under the app data folder
//! (`~/Library/Application Support/com.gears3azura.launcher/` on macOS):
//!
//! - `runtime/azura.toml`: the runtime's own config file, which the runtime reads from beside its
//!   executable. It holds every setting that changes how the game runs, under the runtime's
//!   setting (cvar) names. The in-game settings menu (F4) writes to it too, so the launcher only
//!   ever rewrites its own keys and keeps everything else, comments included.
//! - `launcher.toml`: what only the launcher uses (interface language, chosen preset, game
//!   language until the runtime has a setting for it).
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
const KEY_GPU_PLUGIN: &str = "gpu_plugin";
const KEY_FULLSCREEN: &str = "fullscreen";
const KEY_SCALE_X: &str = "draw_resolution_scale_x";
const KEY_SCALE_Y: &str = "draw_resolution_scale_y";
const KEY_ANISOTROPIC: &str = "anisotropic_override";
const KEY_ANTIALIASING: &str = "swap_post_effect";
const KEY_UPSCALING: &str = "present_effect";
const KEY_TEXTURE_SOFT: &str = "texture_cache_memory_limit_soft";
const KEY_TEXTURE_HARD: &str = "texture_cache_memory_limit_hard";

/// GPU plugin the runtime needs to draw anything; written on every save.
const GPU_PLUGIN: &str = "xenos";

/// Graphics values as stored in `azura.toml`; `None` when the key is absent or has another type.
#[derive(Debug, Default, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct GraphicsValues {
    pub resolution_scale: Option<i64>,
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
}

impl Default for LauncherFile {
    fn default() -> Self {
        Self {
            graphics_preset: "auto".into(),
            ui_language: "auto".into(),
            game_language: None,
            game_language_chosen: false,
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

    /// Loads the settings; missing files and missing keys load as defaults.
    pub fn load(&self) -> Result<Settings, String> {
        let runtime_text = read_optional(&self.runtime_config).map_err(|e| e.to_string())?;
        let runtime: DocumentMut = runtime_text
            .parse()
            .map_err(|e| format!("{} is not valid TOML: {e}", self.runtime_config.display()))?;

        let launcher_text = read_optional(&self.launcher_config).map_err(|e| e.to_string())?;
        let launcher: LauncherFile = toml::from_str(&launcher_text).unwrap_or_default();

        Ok(Settings {
            game_folder: get_string(&runtime, KEY_GAME_DATA_ROOT),
            fullscreen: runtime
                .get(KEY_FULLSCREEN)
                .and_then(Item::as_bool)
                .unwrap_or(false),
            graphics: GraphicsValues {
                resolution_scale: get_integer(&runtime, KEY_SCALE_X),
                anisotropic: get_integer(&runtime, KEY_ANISOTROPIC),
                antialiasing: get_string(&runtime, KEY_ANTIALIASING),
                upscaling: get_string(&runtime, KEY_UPSCALING),
                texture_memory: get_integer(&runtime, KEY_TEXTURE_SOFT),
            },
            graphics_preset: launcher.graphics_preset,
            ui_language: launcher.ui_language,
            game_language: launcher.game_language,
            game_language_chosen: launcher.game_language_chosen,
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
        set_or_remove(&mut runtime, KEY_SCALE_X, graphics.resolution_scale.map(value));
        set_or_remove(&mut runtime, KEY_SCALE_Y, graphics.resolution_scale.map(value));
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
        };
        let launcher_text = toml::to_string(&launcher).map_err(|e| e.to_string())?;
        write_atomically(&self.launcher_config, &launcher_text).map_err(|e| e.to_string())
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
            fullscreen: true,
            graphics: GraphicsValues {
                resolution_scale: Some(2),
                anisotropic: Some(5),
                antialiasing: Some("fxaa".into()),
                upscaling: Some("fsr".into()),
                texture_memory: Some(768),
            },
            graphics_preset: "high".into(),
            ui_language: "es".into(),
            game_language: Some("ESN".into()),
            game_language_chosen: true,
        }
    }

    #[test]
    fn missing_files_load_as_defaults() {
        let dir = temp_folder("defaults");
        let settings = SettingsPaths::new(&dir).load().unwrap();
        assert_eq!(settings.game_folder, None);
        assert!(!settings.fullscreen);
        assert_eq!(settings.graphics, GraphicsValues::default());
        assert_eq!(settings.graphics_preset, "auto");
        assert_eq!(settings.ui_language, "auto");
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
            "fullscreen = true",
            "game_data_root = \"/Games/Gears of War 3\"",
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
        paths.save(&cleared).unwrap();
        let text = fs::read_to_string(dir.join(RUNTIME_CONFIG)).unwrap();
        assert!(!text.contains("game_data_root"));
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
