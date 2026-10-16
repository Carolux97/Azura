//! Helpers shared by the unit tests.

use std::ops::Deref;
use std::path::{Path, PathBuf};

/// An empty folder under the system temp directory, removed with its contents when dropped, so
/// tests leave nothing behind even when they fail.
pub struct TempFolder(PathBuf);

impl TempFolder {
    /// Creates the folder; `name` keeps it unique among the tests of one run.
    pub fn new(name: &str) -> Self {
        let dir = std::env::temp_dir().join(format!("azura-test-{}-{name}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).expect("cannot create a temp folder");
        Self(dir)
    }
}

impl Deref for TempFolder {
    type Target = Path;

    fn deref(&self) -> &Path {
        &self.0
    }
}

impl Drop for TempFolder {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
