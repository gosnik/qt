mod common;

use common::*;
use qt_memory::qtmem::{is_sqlite_database, read_qtmem_v3};
use qt_memory::skill_import::{SkillImportOptions, import_skill_memories};
use qt_memory::*;

/// The qtmem v3 fixture schema used by the archived `memory_probe.cpp`.
const QTMEM_V3_SCHEMA: &str = r#"
    CREATE TABLE meta(key TEXT PRIMARY KEY,value TEXT);
    INSERT INTO meta VALUES('schema_version','3');
    CREATE TABLE memories(
      id TEXT,user TEXT,organization TEXT,project TEXT,repository TEXT,
      branch TEXT,task TEXT,session TEXT,agent_id TEXT,conversation_id TEXT,
      scope_json TEXT,memory_type TEXT,status TEXT,provenance_json TEXT,
      source TEXT,source_type TEXT,source_uri TEXT,source_hash TEXT,
      source_span TEXT,created_by TEXT,content TEXT,confidence REAL,
      evidence_strength REAL,quality_score REAL,success_count INTEGER,
      failure_count INTEGER,last_outcome TEXT,access_count INTEGER,
      superseded_by TEXT,replacement_id TEXT,created_at INTEGER,
      updated_at INTEGER,last_accessed_at INTEGER);
    CREATE TABLE memory_tags(memory_id INTEGER,tag TEXT);
    CREATE TABLE graph_edges(id TEXT,from_id TEXT,to_id TEXT,type TEXT,
      confidence REAL,source TEXT,created_at INTEGER);
    CREATE TABLE procedure_runs(id TEXT,procedure_id TEXT,task_id TEXT,
      outcome TEXT,notes TEXT,evidence_uri TEXT,created_at INTEGER);
    CREATE TABLE memory_revisions(id TEXT,memory_id TEXT,revision_kind TEXT,
      entry_json TEXT,created_at INTEGER);
    CREATE TABLE sources(id TEXT,project TEXT,source_type TEXT,source_uri TEXT,
      source_hash TEXT,display_name TEXT,trust_level TEXT,hash_status TEXT,
      ingest_count INTEGER,imported_at INTEGER,last_ingested_at INTEGER,
      last_checked_at INTEGER);
    CREATE TABLE source_events(id TEXT,source_id TEXT,project TEXT,action TEXT,
      status TEXT,source_hash TEXT,note TEXT,created_at INTEGER);
    CREATE TABLE review_items(id TEXT,user TEXT,organization TEXT,project TEXT,
      repository TEXT,branch TEXT,task TEXT,session TEXT,agent_id TEXT,
      conversation_id TEXT,scope_json TEXT,kind TEXT,status TEXT,priority TEXT,
      reason TEXT,subject_id TEXT,source TEXT,source_type TEXT,source_uri TEXT,
      source_hash TEXT,provenance_json TEXT,candidate_json TEXT,decision TEXT,
      decided_by TEXT,decision_note TEXT,created_at INTEGER,updated_at INTEGER,
      decided_at INTEGER);
    CREATE TABLE review_item_tags(review_item_id INTEGER,tag TEXT);
    CREATE TABLE audit_events(id TEXT,actor TEXT,action TEXT,user TEXT,
      organization TEXT,project TEXT,repository TEXT,branch TEXT,task TEXT,
      session TEXT,agent_id TEXT,conversation_id TEXT,scope_json TEXT,
      reason TEXT,before_id TEXT,after_id TEXT,evidence_uri TEXT,
      metadata_json TEXT,created_at INTEGER);
    CREATE TABLE session_events(id TEXT,user TEXT,organization TEXT,project TEXT,
      repository TEXT,branch TEXT,task TEXT,session TEXT,agent_id TEXT,
      conversation_id TEXT,scope_json TEXT,event_type TEXT,source TEXT,
      payload_json TEXT,created_at INTEGER);
"#;

fn qtmem_fixture(root: &std::path::Path) -> std::path::PathBuf {
    std::fs::create_dir_all(root.join("blocks/block_project")).unwrap();
    let path = root.join("memory.sqlite3");
    let conn = rusqlite::Connection::open(&path).unwrap();
    conn.execute_batch(QTMEM_V3_SCHEMA).unwrap();
    conn.execute_batch(
        "INSERT INTO memories(id,project,memory_type,status,content,source,confidence,created_at,updated_at,replacement_id)
           VALUES('1','block_project','decision','hypothesis','fixture decision','qtmem',0.7,1000,2000,'2');
         INSERT INTO memories(id,project,memory_type,status,content,confidence,created_at,updated_at)
           VALUES('2','block_project','procedure','active','run the tests',0.9,1000,3000);
         INSERT INTO memory_tags VALUES(1,'legacy');
         INSERT INTO graph_edges VALUES('e1','1','2','supports',1.0,'qtmem',1000);
         INSERT INTO review_items(id,project,kind,status,priority,reason,subject_id,candidate_json,created_at)
           VALUES('1','block_project','candidate_memory','pending','normal','check','1','{\"content\":\"x\"}',1000);
         INSERT INTO review_item_tags VALUES(1,'imported');",
    )
    .unwrap();
    drop(conn);
    std::fs::write(
        root.join("blocks/block_project/repo_rules.md"),
        "Use the repository-native build contract.",
    )
    .unwrap();
    path
}

#[test]
fn reads_qtmem_v3_read_only_and_imports() {
    let source = tempfile::tempdir().unwrap();
    let path = qtmem_fixture(source.path());
    assert!(is_sqlite_database(&path));
    let before = (
        std::fs::metadata(&path).unwrap().len(),
        std::fs::metadata(&path).unwrap().modified().unwrap(),
    );
    let package = read_qtmem_v3(&path).unwrap();
    assert_eq!(package.format, "qtmem-sqlite-v3");
    assert_eq!(package.schema_version, 3);
    assert_eq!(package.state["memories"].as_array().unwrap().len(), 2);
    assert_eq!(
        package.state["memories"][0]["tags"],
        serde_json::json!(["legacy"])
    );
    assert_eq!(
        package.state["review_items"][0]["tags"],
        serde_json::json!(["imported"])
    );
    assert_eq!(package.state["blocks"][0]["id"], "repo_rules");
    assert_eq!(
        package.state["blocks"][0]["content"],
        "Use the repository-native build contract."
    );
    let after = (
        std::fs::metadata(&path).unwrap().len(),
        std::fs::metadata(&path).unwrap().modified().unwrap(),
    );
    assert_eq!(before, after, "the source database is never modified");

    let fixture = Fixture::new();
    fixture
        .store
        .import_state(&scope(), &package.state, false)
        .unwrap();
    let memories = fixture
        .store
        .query(
            &scope(),
            &MemoryQuery {
                include_superseded: true,
                ..Default::default()
            },
        )
        .unwrap();
    assert_eq!(memories.len(), 2);
    let procedure = memories
        .iter()
        .find(|memory| memory.content == "run the tests")
        .unwrap();
    assert_eq!(
        procedure.memory_type,
        MemoryType::Procedural,
        "qtmem `procedure` alias"
    );
    let decision = memories
        .iter()
        .find(|memory| memory.content == "fixture decision")
        .unwrap();
    assert_eq!(
        decision.replacement_id, procedure.id,
        "replacement references are remapped"
    );
    assert_eq!(
        fixture
            .store
            .query_edges(&scope(), &GraphQuery::default())
            .unwrap()
            .len(),
        1
    );
    let reviews = fixture
        .store
        .reviews(&scope(), ReviewStatus::Pending, 10)
        .unwrap();
    assert_eq!(reviews[0].subject_id, decision.id);
    assert_eq!(
        fixture
            .store
            .get_block(&scope(), "repo_rules")
            .unwrap()
            .content,
        "Use the repository-native build contract."
    );
}

#[test]
fn rejects_non_qtmem_sources() {
    let dir = tempfile::tempdir().unwrap();
    let text = dir.path().join("notes.txt");
    std::fs::write(&text, "hello").unwrap();
    assert_eq!(
        read_qtmem_v3(&text).unwrap_err().to_string(),
        "qtmem import source is not a SQLite database"
    );
    let fixture = Fixture::new();
    assert_eq!(
        read_qtmem_v3(fixture.store.path()).unwrap_err().to_string(),
        "import source is a native qt memory database, not standalone qtmem v3"
    );
    let partial = dir.path().join("partial.sqlite3");
    rusqlite::Connection::open(&partial)
        .unwrap()
        .execute_batch("CREATE TABLE meta(key TEXT,value TEXT);")
        .unwrap();
    assert_eq!(
        read_qtmem_v3(&partial).unwrap_err().to_string(),
        "qtmem v3 database is missing table: memories"
    );
}

#[test]
fn skill_import_creates_detects_unchanged_and_supersedes() {
    let fixture = Fixture::new();
    let memories = fixture.service();
    let governance = GovernanceService::new(fixture.service(), LifecyclePolicy::default());
    let repository = durable_memory_scope(scope());
    let repository = MemoryScope {
        kind: ScopeKind::Repository,
        branch: String::new(),
        ..repository
    };
    let skills = tempfile::tempdir().unwrap();
    let package = skills.path().join("deploy");
    std::fs::create_dir_all(package.join("scripts")).unwrap();
    std::fs::create_dir_all(package.join("node_modules")).unwrap();
    std::fs::write(package.join("SKILL.md"), "# Deploy\nRun the deploy script.").unwrap();
    std::fs::write(package.join("scripts/deploy.sh"), "echo deploy").unwrap();
    std::fs::write(package.join("node_modules/ignored.js"), "ignored").unwrap();
    std::fs::write(package.join("image.png"), [0u8, 1, 2]).unwrap();

    let options = SkillImportOptions {
        recursive: true,
        ..SkillImportOptions::new(skills.path())
    };
    let report = import_skill_memories(&memories, &governance, &repository, &options).unwrap();
    assert_eq!(
        (report.discovered, report.created, report.failed),
        (1, 1, 0)
    );
    assert_eq!(report.items[0].related_files, 1);
    let memory = memories
        .get(&repository, &report.items[0].memory_id)
        .unwrap();
    assert_eq!(memory.memory_type, MemoryType::Skill);
    assert_eq!(memory.status, MemoryStatus::Active);
    assert!(
        memory
            .content
            .starts_with("# Skill memory: deploy\n\n## SKILL.md\n\n# Deploy")
    );
    assert!(
        memory
            .content
            .contains("## Related: scripts/deploy.sh\n\necho deploy")
    );
    assert!(
        memory
            .content
            .contains("## Package inventory\n\n- scripts/deploy.sh")
    );
    assert!(!memory.content.contains("ignored"));
    assert!(memory.tags.contains(&"skill:name:deploy".to_string()));
    assert!(memory.provenance.source_uri.starts_with("skill://local/"));

    let unchanged = import_skill_memories(&memories, &governance, &repository, &options).unwrap();
    assert_eq!(unchanged.unchanged, 1);
    assert_eq!(unchanged.items[0].memory_id, memory.id);

    std::fs::write(
        package.join("SKILL.md"),
        "# Deploy\nRun the new deploy script.",
    )
    .unwrap();
    let updated = import_skill_memories(&memories, &governance, &repository, &options).unwrap();
    assert_eq!(updated.updated, 1);
    let old = memories.get(&repository, &memory.id).unwrap();
    assert_eq!(old.status, MemoryStatus::Superseded);
    assert_eq!(old.replacement_id, updated.items[0].memory_id);

    assert_eq!(
        import_skill_memories(&memories, &governance, &scope(), &options)
            .unwrap_err()
            .to_string(),
        "skill imports require repository memory scope"
    );
    let outside = SkillImportOptions {
        containment_root: Some(package.join("scripts")),
        ..SkillImportOptions::new(skills.path())
    };
    assert_eq!(
        import_skill_memories(&memories, &governance, &repository, &outside)
            .unwrap_err()
            .to_string(),
        "skill scan root is outside the allowed workspace"
    );
    let non_recursive = SkillImportOptions::new(skills.path());
    assert_eq!(
        import_skill_memories(&memories, &governance, &repository, &non_recursive)
            .unwrap()
            .discovered,
        0
    );
}
