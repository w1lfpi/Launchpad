PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS schema_version (
    version INTEGER PRIMARY KEY,
    applied_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS app_items (
    id INTEGER PRIMARY KEY,
    stable_key TEXT NOT NULL UNIQUE,
    display_name TEXT NOT NULL,
    launch_path TEXT NOT NULL,
    icon_source TEXT,
    last_seen_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    is_hidden INTEGER NOT NULL DEFAULT 0 CHECK (is_hidden IN (0, 1))
);

CREATE TABLE IF NOT EXISTS folders (
    id INTEGER PRIMARY KEY,
    title TEXT NOT NULL,
    page_index INTEGER NOT NULL DEFAULT 0 CHECK (page_index >= 0),
    position_index INTEGER NOT NULL DEFAULT 0 CHECK (position_index >= 0)
);

CREATE TABLE IF NOT EXISTS layout_items (
    id INTEGER PRIMARY KEY,
    app_id INTEGER,
    folder_id INTEGER,
    parent_folder_id INTEGER,
    page_index INTEGER NOT NULL DEFAULT 0 CHECK (page_index >= 0),
    position_index INTEGER NOT NULL CHECK (position_index >= 0),
    CHECK (
        (app_id IS NOT NULL AND folder_id IS NULL) OR
        (app_id IS NULL AND folder_id IS NOT NULL)
    ),
    UNIQUE (parent_folder_id, page_index, position_index),
    FOREIGN KEY (app_id) REFERENCES app_items(id) ON DELETE CASCADE,
    FOREIGN KEY (folder_id) REFERENCES folders(id) ON DELETE CASCADE,
    FOREIGN KEY (parent_folder_id) REFERENCES folders(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS settings (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

INSERT OR IGNORE INTO schema_version(version) VALUES (1);
INSERT OR IGNORE INTO settings(key, value)
VALUES
    ('hotkey', 'Win+Alt+Space'),
    ('columns', '7'),
    ('rows', '5');

CREATE INDEX IF NOT EXISTS idx_app_items_name
    ON app_items(display_name);
CREATE INDEX IF NOT EXISTS idx_layout_page
    ON layout_items(parent_folder_id, page_index, position_index);
