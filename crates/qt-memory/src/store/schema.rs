//! Schema migrations v1–v7, byte-for-byte the SQL of the archived
//! `sqlite_memory_database.cpp`. Existing databases created by the C++ binary
//! open unchanged; never edit an applied migration, add a new one instead.

/// Highest schema version this build understands.
pub const SCHEMA_VERSION: usize = 7;

pub(crate) const MIGRATION_TABLE: &str = "CREATE TABLE IF NOT EXISTS memory_schema_migrations(version INTEGER PRIMARY KEY,applied_at INTEGER NOT NULL);";

pub(crate) const MIGRATION_1: &str = concat!(
    "CREATE TABLE memories(",
    "id TEXT PRIMARY KEY,scope_key TEXT NOT NULL,scope_kind TEXT NOT NULL,",
    "user_id TEXT NOT NULL,organization_id TEXT NOT NULL,project_id TEXT NOT NULL,",
    "repository_id TEXT NOT NULL,repository_display TEXT NOT NULL,branch_id TEXT NOT NULL,",
    "task_id TEXT NOT NULL,agent_id TEXT NOT NULL,session_id TEXT NOT NULL,",
    "conversation_id TEXT NOT NULL,memory_type TEXT NOT NULL,status TEXT NOT NULL,",
    "source TEXT NOT NULL,source_id TEXT NOT NULL,source_type TEXT NOT NULL,",
    "source_uri TEXT NOT NULL,source_hash TEXT NOT NULL,source_span TEXT NOT NULL,",
    "created_by TEXT NOT NULL,evidence_strength REAL NOT NULL,content TEXT NOT NULL,",
    "confidence REAL NOT NULL,quality_score REAL NOT NULL,success_count INTEGER NOT NULL,",
    "failure_count INTEGER NOT NULL,last_outcome TEXT NOT NULL,access_count INTEGER NOT NULL,",
    "superseded_by TEXT NOT NULL,replacement_id TEXT NOT NULL,created_at INTEGER NOT NULL,",
    "updated_at INTEGER NOT NULL,last_accessed_at INTEGER NOT NULL);",
    "CREATE INDEX memories_scope_idx ON memories(scope_key,status,updated_at DESC);",
    "CREATE TABLE memory_tags(memory_id TEXT NOT NULL REFERENCES memories(id) ON DELETE CASCADE,",
    "tag TEXT NOT NULL,PRIMARY KEY(memory_id,tag));",
    "CREATE INDEX memory_tags_tag_idx ON memory_tags(tag,memory_id);",
    "CREATE VIRTUAL TABLE memory_fts USING fts5(memory_id UNINDEXED,content,source,",
    "memory_type UNINDEXED,source_uri,tokenize='unicode61');",
);

pub(crate) const MIGRATION_2: &str = concat!(
    "CREATE TABLE graph_edges(id TEXT PRIMARY KEY,scope_key TEXT NOT NULL,from_id TEXT NOT NULL,",
    "to_id TEXT NOT NULL,type TEXT NOT NULL,confidence REAL NOT NULL,source TEXT NOT NULL,",
    "created_at INTEGER NOT NULL,UNIQUE(scope_key,from_id,to_id,type));",
    "CREATE INDEX graph_edges_scope_idx ON graph_edges(scope_key,from_id,to_id,type);",
    "CREATE TABLE procedure_runs(id TEXT PRIMARY KEY,scope_key TEXT NOT NULL,procedure_id TEXT NOT NULL,",
    "task_id TEXT NOT NULL,outcome TEXT NOT NULL,notes TEXT NOT NULL,evidence_uri TEXT NOT NULL,",
    "created_at INTEGER NOT NULL);",
    "CREATE INDEX procedure_runs_scope_idx ON procedure_runs(scope_key,procedure_id,created_at DESC);",
    "CREATE TABLE memory_revisions(id TEXT PRIMARY KEY,scope_key TEXT NOT NULL,memory_id TEXT NOT NULL,",
    "revision_kind TEXT NOT NULL,entry_json TEXT NOT NULL,created_at INTEGER NOT NULL);",
    "CREATE INDEX memory_revisions_scope_idx ON memory_revisions(scope_key,memory_id,created_at DESC);",
    "CREATE TABLE sources(id TEXT NOT NULL,scope_key TEXT NOT NULL,source_type TEXT NOT NULL,",
    "source_uri TEXT NOT NULL,source_hash TEXT NOT NULL,display_name TEXT NOT NULL,trust_level TEXT NOT NULL,",
    "hash_status TEXT NOT NULL,ingest_count INTEGER NOT NULL,imported_at INTEGER NOT NULL,",
    "last_ingested_at INTEGER NOT NULL,last_checked_at INTEGER NOT NULL,PRIMARY KEY(scope_key,id));",
    "CREATE TABLE source_events(id TEXT PRIMARY KEY,scope_key TEXT NOT NULL,source_id TEXT NOT NULL,",
    "action TEXT NOT NULL,status TEXT NOT NULL,source_hash TEXT NOT NULL,note TEXT NOT NULL,created_at INTEGER NOT NULL);",
    "CREATE INDEX source_events_scope_idx ON source_events(scope_key,source_id,created_at DESC);",
);

pub(crate) const MIGRATION_3: &str = concat!(
    "CREATE TABLE review_items(id TEXT PRIMARY KEY,scope_key TEXT NOT NULL,kind TEXT NOT NULL,status TEXT NOT NULL,",
    "priority TEXT NOT NULL,reason TEXT NOT NULL,subject_id TEXT NOT NULL,source_id TEXT NOT NULL,",
    "source_type TEXT NOT NULL,source_uri TEXT NOT NULL,source_hash TEXT NOT NULL,source_span TEXT NOT NULL,",
    "created_by TEXT NOT NULL,evidence_strength REAL NOT NULL,candidate_json TEXT NOT NULL,decision TEXT NOT NULL,",
    "decided_by TEXT NOT NULL,decision_note TEXT NOT NULL,tags_json TEXT NOT NULL,created_at INTEGER NOT NULL,",
    "updated_at INTEGER NOT NULL,decided_at INTEGER NOT NULL);",
    "CREATE INDEX review_items_scope_idx ON review_items(scope_key,status,created_at DESC);",
    "CREATE TABLE audit_events(id TEXT PRIMARY KEY,scope_key TEXT NOT NULL,actor TEXT NOT NULL,action TEXT NOT NULL,",
    "reason TEXT NOT NULL,before_id TEXT NOT NULL,after_id TEXT NOT NULL,evidence_uri TEXT NOT NULL,",
    "metadata_json TEXT NOT NULL,created_at INTEGER NOT NULL);",
    "CREATE INDEX audit_events_scope_idx ON audit_events(scope_key,created_at DESC);",
    "CREATE TABLE session_events(id TEXT PRIMARY KEY,scope_key TEXT NOT NULL,event_type TEXT NOT NULL,",
    "source TEXT NOT NULL,payload_json TEXT NOT NULL,created_at INTEGER NOT NULL);",
    "CREATE INDEX session_events_scope_idx ON session_events(scope_key,event_type,created_at DESC);",
    "CREATE TABLE context_blocks(scope_key TEXT NOT NULL,id TEXT NOT NULL,content TEXT NOT NULL,",
    "updated_at INTEGER NOT NULL,PRIMARY KEY(scope_key,id));",
);

pub(crate) const MIGRATION_4: &str = concat!(
    "ALTER TABLE session_events ADD COLUMN deduplication_key TEXT NOT NULL DEFAULT '';",
    "CREATE UNIQUE INDEX session_events_dedup_idx ON ",
    "session_events(scope_key,event_type,deduplication_key) ",
    "WHERE deduplication_key<>'';",
    "ALTER TABLE procedure_runs ADD COLUMN deduplication_key TEXT NOT NULL DEFAULT '';",
    "CREATE UNIQUE INDEX procedure_runs_dedup_idx ON ",
    "procedure_runs(scope_key,procedure_id,deduplication_key) ",
    "WHERE deduplication_key<>'';",
);

pub(crate) const MIGRATION_5: &str = concat!(
    "ALTER TABLE review_items ADD COLUMN candidate_version INTEGER NOT NULL DEFAULT 1;",
    "ALTER TABLE review_items ADD COLUMN revision INTEGER NOT NULL DEFAULT 0;",
    "ALTER TABLE review_items ADD COLUMN deferred_until INTEGER NOT NULL DEFAULT 0;",
    "ALTER TABLE review_items ADD COLUMN application_status TEXT NOT NULL DEFAULT 'not_started';",
    "ALTER TABLE review_items ADD COLUMN application_error TEXT NOT NULL DEFAULT '';",
    "ALTER TABLE review_items ADD COLUMN action_key TEXT NOT NULL DEFAULT '';",
    "CREATE INDEX review_items_queue_idx ON review_items(",
    "scope_key,status,priority,kind,deferred_until,created_at,id);",
    "CREATE UNIQUE INDEX review_items_action_key_idx ON review_items(scope_key,action_key) ",
    "WHERE action_key<>'';",
    "CREATE TABLE review_actions(",
    "id TEXT PRIMARY KEY,scope_key TEXT NOT NULL,review_id TEXT NOT NULL,",
    "action_key TEXT NOT NULL,action TEXT NOT NULL,actor TEXT NOT NULL,note TEXT NOT NULL,",
    "before_status TEXT NOT NULL,after_status TEXT NOT NULL,",
    "before_revision INTEGER NOT NULL,after_revision INTEGER NOT NULL,",
    "application_status TEXT NOT NULL,application_error TEXT NOT NULL,",
    "result_json TEXT NOT NULL DEFAULT '{}',replay_count INTEGER NOT NULL DEFAULT 0,",
    "created_at INTEGER NOT NULL,",
    "FOREIGN KEY(review_id) REFERENCES review_items(id));",
    "CREATE UNIQUE INDEX review_actions_key_idx ON review_actions(scope_key,action_key) ",
    "WHERE action_key<>'';",
    "CREATE INDEX review_actions_history_idx ON review_actions(scope_key,created_at DESC,id);",
    "CREATE TABLE review_view_state(scope_key TEXT PRIMARY KEY,state_json TEXT NOT NULL,",
    "updated_at INTEGER NOT NULL);",
);

pub(crate) const MIGRATION_6: &str = r#"
CREATE TABLE retrieval_owners(
 owner_key TEXT PRIMARY KEY,scope_key TEXT NOT NULL,kind TEXT NOT NULL,
 owner_id TEXT NOT NULL,revision INTEGER NOT NULL CHECK(revision>0),
 content TEXT NOT NULL,source_uri TEXT NOT NULL,deleted INTEGER NOT NULL DEFAULT 0);
CREATE INDEX retrieval_owners_scope ON retrieval_owners(scope_key,kind,deleted);
CREATE TABLE capture_candidate_receipts(
 scope_key TEXT NOT NULL,deduplication_key TEXT NOT NULL,canonical_content TEXT NOT NULL,
 memory_id TEXT NOT NULL,PRIMARY KEY(scope_key,deduplication_key));
CREATE TABLE retrieval_jobs(
 id INTEGER PRIMARY KEY AUTOINCREMENT,owner_key TEXT NOT NULL REFERENCES retrieval_owners(owner_key) ON DELETE CASCADE,
 owner_revision INTEGER NOT NULL,representation TEXT NOT NULL,generation TEXT NOT NULL,
 state TEXT NOT NULL DEFAULT 'pending' CHECK(state IN ('pending','leased','complete','failed','superseded')),
 lease_token TEXT NOT NULL DEFAULT '',lease_until INTEGER NOT NULL DEFAULT 0,
 attempts INTEGER NOT NULL DEFAULT 0,next_attempt INTEGER NOT NULL DEFAULT 0,
 error TEXT NOT NULL DEFAULT '',UNIQUE(owner_key,owner_revision,representation,generation));
CREATE INDEX retrieval_jobs_claim ON retrieval_jobs(state,next_attempt,lease_until,id);
CREATE TABLE retrieval_chunks(
 id TEXT PRIMARY KEY,owner_key TEXT NOT NULL REFERENCES retrieval_owners(owner_key) ON DELETE CASCADE,
 owner_revision INTEGER NOT NULL,ordinal INTEGER NOT NULL,content TEXT NOT NULL,
 content_hash TEXT NOT NULL,source_uri TEXT NOT NULL,source_span TEXT NOT NULL,
 render_version TEXT NOT NULL,UNIQUE(owner_key,owner_revision,ordinal,render_version));
CREATE INDEX retrieval_chunks_owner ON retrieval_chunks(owner_key,owner_revision);
CREATE TABLE retrieval_dense_indexes(
 scope_key TEXT NOT NULL,generation TEXT NOT NULL,config_json TEXT NOT NULL,
 state TEXT NOT NULL CHECK(state IN ('building','active','retired')),
 PRIMARY KEY(scope_key,generation));
CREATE UNIQUE INDEX retrieval_dense_active ON retrieval_dense_indexes(scope_key) WHERE state='active';
CREATE TABLE retrieval_vectors(
 chunk_id TEXT NOT NULL REFERENCES retrieval_chunks(id) ON DELETE CASCADE,
 generation TEXT NOT NULL,dimensions INTEGER NOT NULL,vector BLOB NOT NULL,
 PRIMARY KEY(chunk_id,generation));
CREATE INDEX retrieval_vectors_generation ON retrieval_vectors(generation,chunk_id);
CREATE VIRTUAL TABLE retrieval_fts USING fts5(chunk_id UNINDEXED,content,source_uri,tokenize='unicode61');
CREATE TRIGGER retrieval_chunk_insert AFTER INSERT ON retrieval_chunks BEGIN
 INSERT INTO retrieval_fts(chunk_id,content,source_uri) VALUES(NEW.id,NEW.content,NEW.source_uri);
END;
CREATE TRIGGER retrieval_chunk_delete AFTER DELETE ON retrieval_chunks BEGIN
 DELETE FROM retrieval_fts WHERE chunk_id=OLD.id;
END;
CREATE TABLE memory_entities(
 id TEXT PRIMARY KEY,scope_key TEXT NOT NULL,scope_json TEXT NOT NULL,
 kind TEXT NOT NULL,name TEXT NOT NULL);
CREATE INDEX memory_entities_scope ON memory_entities(scope_key,name);
CREATE TABLE memory_entity_aliases(
 entity_id TEXT NOT NULL REFERENCES memory_entities(id) ON DELETE CASCADE,
 alias TEXT NOT NULL COLLATE NOCASE,PRIMARY KEY(entity_id,alias));
CREATE INDEX memory_entity_alias_lookup ON memory_entity_aliases(alias,entity_id);
CREATE TABLE retrieval_code_sources(
 owner_key TEXT PRIMARY KEY REFERENCES retrieval_owners(owner_key) ON DELETE CASCADE,
 repository TEXT NOT NULL,worktree TEXT NOT NULL,path TEXT NOT NULL,
 fingerprint TEXT NOT NULL,snapshot_generation INTEGER NOT NULL,sync_token TEXT NOT NULL);
CREATE TABLE memory_assertions(
 memory_id TEXT NOT NULL REFERENCES memories(id) ON DELETE CASCADE,revision INTEGER NOT NULL,
 subject_id TEXT NOT NULL REFERENCES memory_entities(id),predicate TEXT NOT NULL,
 object_id TEXT NOT NULL,literal_json TEXT NOT NULL,qualifiers_json TEXT NOT NULL,
 positive INTEGER NOT NULL,valid_from INTEGER,valid_to INTEGER,evidence_json TEXT NOT NULL,
 recorded_at INTEGER NOT NULL,historical_json TEXT NOT NULL,
 PRIMARY KEY(memory_id,revision));
CREATE INDEX memory_assertions_subject ON memory_assertions(subject_id,predicate,valid_from,valid_to);
CREATE INDEX memory_assertions_object ON memory_assertions(object_id,predicate);
CREATE TABLE memory_assertion_revisions(
 memory_id TEXT NOT NULL,revision INTEGER NOT NULL,recorded_at INTEGER NOT NULL,
 PRIMARY KEY(memory_id,revision));
CREATE TABLE retrieval_cross_store_links(
 id TEXT PRIMARY KEY,scope_key TEXT NOT NULL,scope_json TEXT NOT NULL,
 from_kind TEXT NOT NULL,from_id TEXT NOT NULL,from_revision INTEGER NOT NULL DEFAULT 0,
 from_repository TEXT NOT NULL,from_worktree TEXT NOT NULL,from_path TEXT NOT NULL,
 from_fingerprint TEXT NOT NULL,from_generation INTEGER NOT NULL DEFAULT 0,
 to_kind TEXT NOT NULL,to_id TEXT NOT NULL,to_revision INTEGER NOT NULL DEFAULT 0,
 to_repository TEXT NOT NULL,to_worktree TEXT NOT NULL,to_path TEXT NOT NULL,
 to_fingerprint TEXT NOT NULL,to_generation INTEGER NOT NULL DEFAULT 0,
 relation TEXT NOT NULL,evidence_id TEXT NOT NULL,created_at INTEGER NOT NULL);
CREATE INDEX retrieval_cross_store_links_from
 ON retrieval_cross_store_links(scope_key,from_kind,from_id,relation,id);
CREATE INDEX retrieval_cross_store_links_to
 ON retrieval_cross_store_links(scope_key,to_kind,to_id,relation,id);
CREATE INDEX graph_edges_reverse_idx ON graph_edges(scope_key,to_id,type,from_id);
INSERT INTO retrieval_owners(owner_key,scope_key,kind,owner_id,revision,content,source_uri)
 SELECT 'memory:'||id,scope_key,'memory',id,1,content,source_uri FROM memories;
INSERT INTO retrieval_jobs(owner_key,owner_revision,representation,generation)
 SELECT owner_key,revision,'chunks','text-v1' FROM retrieval_owners;
INSERT INTO memory_assertion_revisions(memory_id,revision,recorded_at)
 SELECT id,1,created_at FROM memories;
CREATE TRIGGER retrieval_memory_insert AFTER INSERT ON memories BEGIN
 INSERT INTO retrieval_owners(owner_key,scope_key,kind,owner_id,revision,content,source_uri)
 VALUES('memory:'||NEW.id,NEW.scope_key,'memory',NEW.id,1,NEW.content,NEW.source_uri);
 INSERT INTO retrieval_jobs(owner_key,owner_revision,representation,generation)
 VALUES('memory:'||NEW.id,1,'chunks','text-v1');
 INSERT INTO memory_assertion_revisions(memory_id,revision,recorded_at)
 VALUES(NEW.id,1,NEW.created_at);
END;
CREATE TRIGGER retrieval_memory_update AFTER UPDATE OF
 content,memory_type,status,source_id,source_type,source_uri,source_hash,source_span,
 created_by,evidence_strength,superseded_by,replacement_id,scope_key ON memories BEGIN
 UPDATE retrieval_owners SET revision=revision+1,content=NEW.content,
 source_uri=NEW.source_uri,scope_key=NEW.scope_key WHERE owner_key='memory:'||NEW.id;
 INSERT INTO memory_assertions
 SELECT a.memory_id,o.revision,a.subject_id,a.predicate,a.object_id,a.literal_json,
        a.qualifiers_json,a.positive,a.valid_from,a.valid_to,a.evidence_json,NEW.updated_at,
        json_object('content',NEW.content,'source',NEW.source,'type',NEW.memory_type,
          'status',NEW.status,'source_id',NEW.source_id,'source_type',NEW.source_type,
          'source_uri',NEW.source_uri,'source_hash',NEW.source_hash,'source_span',NEW.source_span,
          'created_by',NEW.created_by,'evidence_strength',NEW.evidence_strength,
          'created_at',NEW.created_at,'updated_at',NEW.updated_at)
 FROM memory_assertions a JOIN retrieval_owners o ON o.owner_key='memory:'||a.memory_id
 WHERE a.memory_id=NEW.id AND a.revision=o.revision-1
   AND NEW.content=OLD.content AND NEW.scope_key=OLD.scope_key
   AND NEW.source_id=OLD.source_id AND NEW.source_type=OLD.source_type
   AND NEW.source_uri=OLD.source_uri AND NEW.source_hash=OLD.source_hash
   AND NEW.source_span=OLD.source_span AND NEW.created_by=OLD.created_by;
 INSERT INTO memory_assertion_revisions(memory_id,revision,recorded_at)
 SELECT NEW.id,o.revision,NEW.updated_at FROM retrieval_owners o
 WHERE o.owner_key='memory:'||NEW.id;
 DELETE FROM retrieval_chunks WHERE owner_key='memory:'||NEW.id;
 UPDATE retrieval_jobs SET state='superseded',lease_token='',lease_until=0
 WHERE owner_key='memory:'||NEW.id AND state IN ('pending','leased','failed');
 INSERT INTO retrieval_jobs(owner_key,owner_revision,representation,generation)
 SELECT owner_key,revision,'chunks','text-v1' FROM retrieval_owners WHERE owner_key='memory:'||NEW.id;
END;
CREATE TRIGGER retrieval_memory_delete AFTER DELETE ON memories BEGIN
 UPDATE retrieval_owners SET revision=revision+1,deleted=1,content='',source_uri=''
 WHERE owner_key='memory:'||OLD.id;
 DELETE FROM retrieval_chunks WHERE owner_key='memory:'||OLD.id;
 UPDATE retrieval_jobs SET state='superseded',lease_token='',lease_until=0
 WHERE owner_key='memory:'||OLD.id;
END;
"#;

// The retry policy is part of the durable job contract: a worker can crash
// after claiming a lease and before it has a chance to report a failure.
// Existing jobs retain the historical default ceiling.
pub(crate) const MIGRATION_7: &str = "ALTER TABLE retrieval_jobs ADD COLUMN max_attempts INTEGER NOT NULL DEFAULT 5 CHECK(max_attempts BETWEEN 1 AND 100);";

/// Migration SQL indexed by `version - 1`.
pub const MIGRATIONS: [&str; SCHEMA_VERSION] = [
    MIGRATION_1,
    MIGRATION_2,
    MIGRATION_3,
    MIGRATION_4,
    MIGRATION_5,
    MIGRATION_6,
    MIGRATION_7,
];
