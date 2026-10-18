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

use serde::Serialize;

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
