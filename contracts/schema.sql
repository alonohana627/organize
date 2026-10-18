-- End-state reference schema, version 1. Implement feature-sized forward migrations.
-- Runtime additionally configures WAL/synchronous/cache budgets outside this script.
PRAGMA foreign_keys = ON;
BEGIN IMMEDIATE;
CREATE TABLE schema_migrations (
 version INTEGER PRIMARY KEY, checksum TEXT NOT NULL, applied_at INTEGER NOT NULL
) STRICT;
CREATE TABLE library_state (
 singleton INTEGER PRIMARY KEY CHECK(singleton=1), library_uuid TEXT NOT NULL UNIQUE,
 name TEXT NOT NULL, schema_version INTEGER NOT NULL, catalog_revision INTEGER NOT NULL DEFAULT 0 CHECK(catalog_revision>=0),
 created_at INTEGER NOT NULL, normalization_version TEXT NOT NULL
) STRICT;
CREATE TABLE roots (
 id INTEGER PRIMARY KEY, uuid TEXT NOT NULL UNIQUE, name TEXT NOT NULL,
 mode TEXT NOT NULL CHECK(mode IN ('referenced','managed')),
 relative_base TEXT, volume_hint TEXT, case_sensitive INTEGER NOT NULL DEFAULT 0 CHECK(case_sensitive IN (0,1)),
 availability TEXT NOT NULL DEFAULT 'unknown' CHECK(availability IN ('online','offline','unknown','denied')),
 last_seen_at INTEGER, scan_generation INTEGER NOT NULL DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0 CHECK(deleted IN (0,1))
) STRICT;
CREATE TABLE folders (
 id INTEGER PRIMARY KEY, root_id INTEGER NOT NULL REFERENCES roots(id), parent_id INTEGER REFERENCES folders(id),
 relative_path TEXT NOT NULL, path_key TEXT NOT NULL, UNIQUE(root_id,path_key), CHECK(parent_id IS NULL OR parent_id<>id)
) STRICT;
CREATE INDEX folders_parent ON folders(root_id,parent_id,id);
CREATE TABLE categories (
 id INTEGER PRIMARY KEY, uuid TEXT NOT NULL UNIQUE, parent_id INTEGER REFERENCES categories(id),
 name TEXT NOT NULL, name_key TEXT NOT NULL, revision INTEGER NOT NULL DEFAULT 0,
 deleted INTEGER NOT NULL DEFAULT 0 CHECK(deleted IN (0,1)), CHECK(parent_id IS NULL OR parent_id<>id)
) STRICT;
CREATE UNIQUE INDEX categories_sibling ON categories(COALESCE(parent_id,0),name_key) WHERE deleted=0;
CREATE TABLE samples (
 id INTEGER PRIMARY KEY, uuid TEXT NOT NULL UNIQUE, display_name TEXT NOT NULL, name_key TEXT NOT NULL,
 content_revision INTEGER NOT NULL DEFAULT 1 CHECK(content_revision>=1), edit_revision INTEGER NOT NULL DEFAULT 0,
 favorite INTEGER NOT NULL DEFAULT 0 CHECK(favorite IN (0,1)), rating INTEGER NOT NULL DEFAULT 0 CHECK(rating BETWEEN 0 AND 5),
 notes TEXT NOT NULL DEFAULT '', category_id INTEGER REFERENCES categories(id),
 bpm_override_state TEXT NOT NULL DEFAULT 'auto' CHECK(bpm_override_state IN ('auto','value','unknown')),
 bpm_override_milli INTEGER CHECK(bpm_override_milli BETWEEN 20000 AND 400000),
 key_override_state TEXT NOT NULL DEFAULT 'auto' CHECK(key_override_state IN ('auto','value','unknown')),
 key_override_class INTEGER CHECK(key_override_class BETWEEN 0 AND 11),
 key_override_mode TEXT CHECK(key_override_mode IN ('major','minor','unknown')),
 created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL, deleted INTEGER NOT NULL DEFAULT 0 CHECK(deleted IN (0,1)),
 CHECK((bpm_override_state='value' AND bpm_override_milli IS NOT NULL) OR (bpm_override_state<>'value' AND bpm_override_milli IS NULL)),
 CHECK((key_override_state='value' AND key_override_class IS NOT NULL AND key_override_mode IS NOT NULL) OR
       (key_override_state<>'value' AND key_override_class IS NULL AND key_override_mode IS NULL))
) STRICT;
CREATE INDEX samples_name ON samples(deleted,name_key,id);
CREATE INDEX samples_added ON samples(deleted,created_at,id);
CREATE INDEX samples_rating ON samples(deleted,rating,id);
CREATE INDEX samples_category ON samples(category_id,deleted,id);
CREATE TABLE locations (
 id INTEGER PRIMARY KEY, sample_id INTEGER NOT NULL REFERENCES samples(id), root_id INTEGER NOT NULL REFERENCES roots(id),
 folder_id INTEGER REFERENCES folders(id), relative_path TEXT NOT NULL, path_key TEXT NOT NULL,
 observed_volume_id TEXT, file_id BLOB, byte_size INTEGER CHECK(byte_size>=0), mtime_ns INTEGER,
 state TEXT NOT NULL CHECK(state IN ('present','missing','unknown','denied','changed','retired')),
 observed_content_revision INTEGER NOT NULL, seen_scan_generation INTEGER NOT NULL DEFAULT 0,
 last_seen_at INTEGER, byte_hash BLOB, hash_algorithm TEXT,
 CHECK(length(relative_path)>0), CHECK(byte_hash IS NULL OR hash_algorithm IS NOT NULL)
) STRICT;
CREATE UNIQUE INDEX locations_live_path ON locations(root_id,path_key) WHERE state<>'retired';
CREATE INDEX locations_sample ON locations(sample_id,state,id);
CREATE INDEX locations_folder ON locations(folder_id,sample_id);
CREATE INDEX locations_identity ON locations(root_id,observed_volume_id,file_id);
CREATE INDEX locations_hash ON locations(hash_algorithm,byte_hash) WHERE byte_hash IS NOT NULL;
CREATE TABLE metadata_observations (
 id INTEGER PRIMARY KEY, sample_id INTEGER NOT NULL REFERENCES samples(id), content_revision INTEGER NOT NULL,
 field TEXT NOT NULL, value_json TEXT NOT NULL CHECK(json_valid(value_json)),
 source TEXT NOT NULL CHECK(source IN ('container','filename','analysis','accepted_suggestion')),
 algorithm_version TEXT, profile_digest TEXT, confidence REAL CHECK(confidence IS NULL OR confidence BETWEEN 0 AND 1),
 created_at INTEGER NOT NULL, valid INTEGER NOT NULL DEFAULT 1 CHECK(valid IN (0,1))
) STRICT;
CREATE INDEX observations_sample ON metadata_observations(sample_id,field,content_revision,valid);
CREATE TABLE effective_metadata (
 sample_id INTEGER PRIMARY KEY REFERENCES samples(id), content_revision INTEGER NOT NULL,
 format TEXT, codec TEXT, duration_us INTEGER CHECK(duration_us>=0), duration_exact INTEGER NOT NULL DEFAULT 0 CHECK(duration_exact IN (0,1)),
 source_frames INTEGER CHECK(source_frames>=0), sample_rate INTEGER CHECK(sample_rate>0), channels INTEGER CHECK(channels>0),
 bit_depth INTEGER CHECK(bit_depth>0), byte_size INTEGER CHECK(byte_size>=0),
 bpm_milli INTEGER CHECK(bpm_milli BETWEEN 20000 AND 400000), bpm_source TEXT, bpm_confidence REAL CHECK(bpm_confidence BETWEEN 0 AND 1),
 key_class INTEGER CHECK(key_class BETWEEN 0 AND 11), key_mode TEXT CHECK(key_mode IN ('major','minor','unknown')),
 key_source TEXT, key_confidence REAL CHECK(key_confidence BETWEEN 0 AND 1),
 analysis_flags INTEGER NOT NULL DEFAULT 0
) STRICT;
CREATE INDEX effective_duration ON effective_metadata(duration_us,sample_id);
CREATE INDEX effective_bpm ON effective_metadata(bpm_milli,sample_id);
CREATE INDEX effective_key ON effective_metadata(key_class,key_mode,sample_id);
CREATE INDEX effective_format ON effective_metadata(format,sample_id);
CREATE INDEX effective_size ON effective_metadata(byte_size,sample_id);
CREATE INDEX effective_rate ON effective_metadata(sample_rate,sample_id);
CREATE TABLE tags (
 id INTEGER PRIMARY KEY, uuid TEXT NOT NULL UNIQUE, parent_id INTEGER REFERENCES tags(id), name TEXT NOT NULL, name_key TEXT NOT NULL,
 revision INTEGER NOT NULL DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0 CHECK(deleted IN (0,1)), CHECK(parent_id IS NULL OR parent_id<>id)
) STRICT;
CREATE UNIQUE INDEX tags_sibling ON tags(COALESCE(parent_id,0),name_key) WHERE deleted=0;
CREATE TABLE sample_tags (
 sample_id INTEGER NOT NULL REFERENCES samples(id), tag_id INTEGER NOT NULL REFERENCES tags(id), created_at INTEGER NOT NULL,
 PRIMARY KEY(sample_id,tag_id)
) STRICT;
CREATE INDEX sample_tags_reverse ON sample_tags(tag_id,sample_id);
CREATE TABLE tag_aliases (
 source_tag_id INTEGER PRIMARY KEY REFERENCES tags(id), canonical_tag_id INTEGER NOT NULL REFERENCES tags(id),
 CHECK(source_tag_id<>canonical_tag_id)
) STRICT;
CREATE TABLE collections (
 id INTEGER PRIMARY KEY, uuid TEXT NOT NULL UNIQUE, name TEXT NOT NULL, kind TEXT NOT NULL CHECK(kind IN ('manual','smart')),
 query_json TEXT CHECK(query_json IS NULL OR json_valid(query_json)), query_version INTEGER,
 revision INTEGER NOT NULL DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0 CHECK(deleted IN (0,1)),
 CHECK((kind='manual' AND query_json IS NULL) OR (kind='smart' AND query_json IS NOT NULL AND query_version IS NOT NULL))
) STRICT;
CREATE TABLE collection_items (
 collection_id INTEGER NOT NULL REFERENCES collections(id), sample_id INTEGER NOT NULL REFERENCES samples(id),
 ordinal INTEGER NOT NULL, PRIMARY KEY(collection_id,sample_id), UNIQUE(collection_id,ordinal)
) STRICT;
CREATE TABLE analysis_profiles (
 id INTEGER PRIMARY KEY, uuid TEXT NOT NULL, version INTEGER NOT NULL, name TEXT NOT NULL,
 config_json TEXT NOT NULL CHECK(json_valid(config_json)), digest TEXT NOT NULL UNIQUE,
 created_at INTEGER NOT NULL, UNIQUE(uuid,version)
) STRICT;
CREATE TABLE analysis_runs (
 id INTEGER PRIMARY KEY, uuid TEXT NOT NULL UNIQUE, profile_id INTEGER NOT NULL REFERENCES analysis_profiles(id),
 state TEXT NOT NULL CHECK(state IN ('preparing','queued','running','pausing','paused','cancelling','cancelled','completed','completed_with_errors')),
 requested_at INTEGER NOT NULL, input_revision INTEGER NOT NULL, selection_json TEXT NOT NULL CHECK(json_valid(selection_json)),
 enumerated_count INTEGER NOT NULL DEFAULT 0, cancel_epoch INTEGER NOT NULL DEFAULT 0
) STRICT;
CREATE TABLE jobs (
 id INTEGER PRIMARY KEY, uuid TEXT NOT NULL UNIQUE, run_id INTEGER NOT NULL REFERENCES analysis_runs(id),
 sample_id INTEGER NOT NULL REFERENCES samples(id), content_revision INTEGER NOT NULL, stage TEXT NOT NULL,
 state TEXT NOT NULL CHECK(state IN ('queued','running','blocked','paused','completed','failed','cancelled','interrupted','stale')),
 priority INTEGER NOT NULL DEFAULT 0, attempt INTEGER NOT NULL DEFAULT 0, lease_token TEXT, lease_expires_at INTEGER,
 checkpoint_json TEXT CHECK(checkpoint_json IS NULL OR json_valid(checkpoint_json)), error_code TEXT,
 UNIQUE(run_id,sample_id,content_revision,stage)
) STRICT;
CREATE INDEX jobs_admission ON jobs(state,priority,id);
CREATE INDEX jobs_run ON jobs(run_id,state,id);
CREATE TABLE artifact_segments (
 id INTEGER PRIMARY KEY, uuid TEXT NOT NULL UNIQUE, relative_path TEXT NOT NULL UNIQUE, byte_size INTEGER NOT NULL CHECK(byte_size>=0),
 checksum TEXT NOT NULL, state TEXT NOT NULL CHECK(state IN ('staging','published','retired'))
) STRICT;
CREATE TABLE artifacts (
 id INTEGER PRIMARY KEY, sample_id INTEGER NOT NULL REFERENCES samples(id), content_revision INTEGER NOT NULL,
 kind TEXT NOT NULL, algorithm_version TEXT NOT NULL, profile_digest TEXT NOT NULL,
 segment_id INTEGER REFERENCES artifact_segments(id), offset_bytes INTEGER CHECK(offset_bytes>=0), length_bytes INTEGER CHECK(length_bytes>=0),
 checksum TEXT, summary_json TEXT CHECK(summary_json IS NULL OR json_valid(summary_json)),
 state TEXT NOT NULL CHECK(state IN ('ready','stale','missing','failed')),
 UNIQUE(sample_id,content_revision,kind,algorithm_version,profile_digest)
) STRICT;
CREATE TABLE vector_spaces (
 id INTEGER PRIMARY KEY, digest TEXT NOT NULL UNIQUE, model_digest TEXT NOT NULL, preprocessing_digest TEXT NOT NULL,
 dimensions INTEGER NOT NULL CHECK(dimensions>0), metric TEXT NOT NULL CHECK(metric IN ('cosine','l2')),
 manifest_json TEXT NOT NULL CHECK(json_valid(manifest_json))
) STRICT;
CREATE TABLE vector_entries (
 id INTEGER PRIMARY KEY, sample_id INTEGER NOT NULL REFERENCES samples(id), content_revision INTEGER NOT NULL,
 space_id INTEGER NOT NULL REFERENCES vector_spaces(id), artifact_id INTEGER NOT NULL REFERENCES artifacts(id),
 segment_index INTEGER NOT NULL DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0 CHECK(deleted IN (0,1)),
 UNIQUE(sample_id,content_revision,space_id,segment_index)
) STRICT;
CREATE INDEX vectors_live ON vector_entries(space_id,deleted,sample_id);
CREATE TABLE index_generations (
 id INTEGER PRIMARY KEY, space_id INTEGER NOT NULL REFERENCES vector_spaces(id), generation INTEGER NOT NULL,
 manifest_path TEXT NOT NULL, manifest_checksum TEXT NOT NULL, covered_revision INTEGER NOT NULL,
 state TEXT NOT NULL CHECK(state IN ('building','ready','active','retired','failed')), UNIQUE(space_id,generation)
) STRICT;
CREATE UNIQUE INDEX index_one_active ON index_generations(space_id) WHERE state='active';
CREATE TABLE duplicate_edges (
 sample_a INTEGER NOT NULL REFERENCES samples(id), sample_b INTEGER NOT NULL REFERENCES samples(id),
 revision_a INTEGER NOT NULL, revision_b INTEGER NOT NULL, kind TEXT NOT NULL CHECK(kind IN ('byte_exact','pcm_equal','near')),
 algorithm_version TEXT NOT NULL, score REAL, evidence_json TEXT NOT NULL CHECK(json_valid(evidence_json)),
 confirmed INTEGER NOT NULL DEFAULT 0 CHECK(confirmed IN (0,1)), CHECK(sample_a<sample_b),
 PRIMARY KEY(sample_a,sample_b,revision_a,revision_b,kind,algorithm_version)
) STRICT;
CREATE TABLE history_actions (
 id INTEGER PRIMARY KEY, uuid TEXT NOT NULL UNIQUE, label TEXT NOT NULL, command_version INTEGER NOT NULL,
 state TEXT NOT NULL CHECK(state IN ('applying','applied','partial','undoing','undone','redoing','superseded','conflict')),
 created_at INTEGER NOT NULL, completed_at INTEGER
) STRICT;
CREATE TABLE history_chunks (
 action_id INTEGER NOT NULL REFERENCES history_actions(id), chunk_index INTEGER NOT NULL,
 forward_json TEXT NOT NULL CHECK(json_valid(forward_json)), inverse_json TEXT NOT NULL CHECK(json_valid(inverse_json)),
 state TEXT NOT NULL CHECK(state IN ('applied','undone','conflict')), catalog_revision INTEGER NOT NULL,
 PRIMARY KEY(action_id,chunk_index)
) STRICT;
CREATE TABLE history_state (
 singleton INTEGER PRIMARY KEY CHECK(singleton=1), cursor_action_id INTEGER REFERENCES history_actions(id),
 active_action_id INTEGER REFERENCES history_actions(id)
) STRICT;
CREATE TABLE file_operations (
 id INTEGER PRIMARY KEY, uuid TEXT NOT NULL UNIQUE, kind TEXT NOT NULL,
 state TEXT NOT NULL CHECK(state IN ('intent','copying','verified','published','registered','cancelled','review','failed')),
 manifest_json TEXT NOT NULL CHECK(json_valid(manifest_json)), updated_at INTEGER NOT NULL
) STRICT;
CREATE TABLE review_items (
 id INTEGER PRIMARY KEY, uuid TEXT NOT NULL UNIQUE, kind TEXT NOT NULL, sample_id INTEGER REFERENCES samples(id),
 evidence_json TEXT NOT NULL CHECK(json_valid(evidence_json)), state TEXT NOT NULL CHECK(state IN ('open','accepted','rejected','resolved')),
 created_at INTEGER NOT NULL
) STRICT;
CREATE TABLE scan_sessions (
 id INTEGER PRIMARY KEY, root_id INTEGER NOT NULL REFERENCES roots(id), generation INTEGER NOT NULL,
 state TEXT NOT NULL CHECK(state IN ('running','paused','cancelled','completed','failed')),
 complete_coverage INTEGER NOT NULL DEFAULT 0 CHECK(complete_coverage IN (0,1)), started_at INTEGER NOT NULL,
 UNIQUE(root_id,generation)
) STRICT;
CREATE TABLE scan_frontier (
 scan_id INTEGER NOT NULL REFERENCES scan_sessions(id), relative_directory TEXT NOT NULL,
 state TEXT NOT NULL CHECK(state IN ('queued','running','complete','denied','failed')),
 PRIMARY KEY(scan_id,relative_directory)
) STRICT;
CREATE TABLE catalog_outbox (
 revision INTEGER PRIMARY KEY, envelope_version INTEGER NOT NULL, payload_json TEXT NOT NULL CHECK(json_valid(payload_json)),
 created_at INTEGER NOT NULL
) STRICT;
CREATE TABLE consumer_cursors (
 name TEXT PRIMARY KEY, revision INTEGER NOT NULL, baseline_required INTEGER NOT NULL DEFAULT 0 CHECK(baseline_required IN (0,1))
) STRICT;
CREATE TABLE library_settings (
 key TEXT PRIMARY KEY, version INTEGER NOT NULL, value_json TEXT NOT NULL CHECK(json_valid(value_json))
) STRICT;
CREATE TABLE sample_search (
 sample_id INTEGER PRIMARY KEY REFERENCES samples(id), name TEXT NOT NULL, path TEXT NOT NULL,
 tags TEXT NOT NULL, category TEXT NOT NULL, notes TEXT NOT NULL, embedded TEXT NOT NULL
) STRICT;
CREATE VIRTUAL TABLE sample_fts USING fts5(
 name,path,tags,category,notes,embedded,content='sample_search',content_rowid='sample_id',
 tokenize='unicode61 remove_diacritics 2',prefix='2 3'
);
CREATE TRIGGER sample_search_ai AFTER INSERT ON sample_search BEGIN
 INSERT INTO sample_fts(rowid,name,path,tags,category,notes,embedded)
 VALUES(new.sample_id,new.name,new.path,new.tags,new.category,new.notes,new.embedded);
END;
CREATE TRIGGER sample_search_ad AFTER DELETE ON sample_search BEGIN
 INSERT INTO sample_fts(sample_fts,rowid,name,path,tags,category,notes,embedded)
 VALUES('delete',old.sample_id,old.name,old.path,old.tags,old.category,old.notes,old.embedded);
END;
CREATE TRIGGER sample_search_au AFTER UPDATE ON sample_search BEGIN
 INSERT INTO sample_fts(sample_fts,rowid,name,path,tags,category,notes,embedded)
 VALUES('delete',old.sample_id,old.name,old.path,old.tags,old.category,old.notes,old.embedded);
 INSERT INTO sample_fts(rowid,name,path,tags,category,notes,embedded)
 VALUES(new.sample_id,new.name,new.path,new.tags,new.category,new.notes,new.embedded);
END;
PRAGMA user_version=1;
COMMIT;
