#!/usr/bin/env python3
"""Validate specification contracts. Does not test a nonexistent application."""
from pathlib import Path
import json
import re
import sqlite3
import unittest

ROOT = Path(__file__).resolve().parents[1]
DOCS = sorted((ROOT / 'docs').glob('[0-9][0-9]_*.md'))

class DocumentContracts(unittest.TestCase):
    def test_nine_documents(self):
        self.assertEqual(len(DOCS), 9)
        self.assertEqual([p.name[:2] for p in DOCS], [f'{i:02}' for i in range(1, 10)])

    def test_requirement_traceability(self):
        reqs = (ROOT/'docs/01_REQUIREMENTS.md').read_text(encoding='utf-8')
        roadmap = (ROOT/'docs/07_ROADMAP.md').read_text(encoding='utf-8').split('## 10. Requirement traceability')[1]
        ids = set(re.findall(r'\bR\d{2}\b', reqs))
        self.assertEqual(ids, {f'R{i:02}' for i in range(1,29)})
        self.assertTrue(ids <= set(re.findall(r'\bR\d{2}\b', roadmap)))

    def test_json_examples(self):
        examples = 0
        for p in DOCS:
            for block in re.findall(r'```json\n(.*?)\n```', p.read_text(encoding='utf-8'), re.S):
                self.assertIsInstance(json.loads(block), dict, p.name)
                examples += 1
        self.assertGreaterEqual(examples, 2)

    def test_source_ids_defined(self):
        all_text = '\n'.join(p.read_text(encoding='utf-8') for p in DOCS)
        refs = set(re.findall(r'\bS\d{2}\b', all_text))
        sources = set(re.findall(r'^\| (S\d{2}) \|', (ROOT/'docs/09_DECISIONS_AND_SOURCES.md').read_text(encoding='utf-8'), re.M))
        self.assertEqual(refs, sources)

    def test_fences_and_no_placeholders(self):
        for p in DOCS:
            text = p.read_text(encoding='utf-8')
            self.assertEqual(len(re.findall(r'^```', text, re.M)) % 2, 0, p.name)
            self.assertNotRegex(text, r'\b(?:TODO|FIXME|TBD)\b')

    def test_referenced_repo_files(self):
        for name in re.findall(r'`((?:docs|contracts|tools)/[^`]+\.(?:md|sql|py))`', (ROOT/'README.md').read_text(encoding='utf-8')):
            self.assertTrue((ROOT/name).is_file(), name)

class SchemaContracts(unittest.TestCase):
    def setUp(self):
        self.db = sqlite3.connect(':memory:')
        self.db.executescript((ROOT/'contracts/schema.sql').read_text(encoding='utf-8'))
        self.db.execute("INSERT INTO roots(id,uuid,name,mode) VALUES(1,'root-1','Test','referenced')")
        self.add_sample(1, 'kick')
        self.db.commit()

    def tearDown(self):
        self.db.close()

    def add_sample(self, sid, name):
        self.db.execute('INSERT INTO samples(id,uuid,display_name,name_key,created_at,updated_at) VALUES(?,?,?,?,0,0)',
                        (sid, f'sample-{sid}', name, name))

    def add_search(self, sid, name):
        self.db.execute("INSERT INTO sample_search VALUES(?,?,?,'Drums','','','')", (sid,name,'Packs/'+name))

    def matches(self, query):
        return [r[0] for r in self.db.execute('SELECT rowid FROM sample_fts WHERE sample_fts MATCH ?', (query,))]

    def test_integrity_and_version(self):
        self.assertEqual(self.db.execute('PRAGMA integrity_check').fetchone()[0], 'ok')
        self.assertEqual(self.db.execute('PRAGMA foreign_key_check').fetchall(), [])
        self.assertEqual(self.db.execute('PRAGMA user_version').fetchone()[0], 1)
        self.assertEqual(self.db.execute('PRAGMA foreign_keys').fetchone()[0], 1)

    def test_all_domain_tables_created(self):
        actual={r[0] for r in self.db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
        expected=set(re.findall(r'CREATE TABLE (\w+)', (ROOT/'contracts/schema.sql').read_text(encoding='utf-8')))
        self.assertTrue(expected <= actual)
        self.assertGreaterEqual(len(expected), 30)

    def test_fts_insert_update_delete(self):
        self.add_search(1,'kick')
        self.assertEqual(self.matches('kick'), [1])
        self.db.execute("UPDATE sample_search SET name='snare',path='Packs/snare' WHERE sample_id=1")
        self.assertEqual(self.matches('kick'), [])
        self.assertEqual(self.matches('snare'), [1])
        self.db.execute('DELETE FROM sample_search WHERE sample_id=1')
        self.assertEqual(self.matches('snare'), [])

    def test_fts_rebuild_and_external_integrity(self):
        self.add_search(1,'kick')
        self.db.execute("INSERT INTO sample_fts(sample_fts) VALUES('rebuild')")
        self.db.execute("INSERT INTO sample_fts(sample_fts,rank) VALUES('integrity-check',1)")
        self.assertEqual(self.matches('kick'),[1])

    def test_transaction_rollback_includes_fts(self):
        self.add_search(1,'kick')
        self.db.commit()
        self.db.execute('BEGIN IMMEDIATE')
        self.db.execute("UPDATE samples SET notes='temporary' WHERE id=1")
        self.db.execute("UPDATE sample_search SET notes='temporary' WHERE sample_id=1")
        self.assertEqual(self.matches('temporary'),[1])
        self.db.rollback()
        self.assertEqual(self.matches('temporary'),[])
        self.assertEqual(self.db.execute('SELECT notes FROM samples WHERE id=1').fetchone()[0],'')

    def test_rating_and_strict_type(self):
        for value in (6, -1, 'invalid'):
            with self.assertRaises(sqlite3.IntegrityError):
                self.db.execute('UPDATE samples SET rating=? WHERE id=1',(value,))

    def test_override_state_and_unknown(self):
        with self.assertRaises(sqlite3.IntegrityError):
            self.db.execute("UPDATE samples SET bpm_override_state='value' WHERE id=1")
        self.db.execute("UPDATE samples SET bpm_override_state='value', bpm_override_milli=155000 WHERE id=1")
        self.db.execute("UPDATE samples SET bpm_override_state='unknown', bpm_override_milli=NULL WHERE id=1")
        self.assertEqual(self.db.execute('SELECT bpm_override_state,bpm_override_milli FROM samples').fetchone(),('unknown',None))
        with self.assertRaises(sqlite3.IntegrityError):
            self.db.execute("UPDATE samples SET key_override_state='value',key_override_class=12,key_override_mode='minor' WHERE id=1")

    def test_foreign_keys(self):
        with self.assertRaises(sqlite3.IntegrityError):
            self.db.execute('INSERT INTO sample_tags VALUES(1,999,0)')

    def test_tag_root_sibling_uniqueness(self):
        self.db.execute("INSERT INTO tags(id,uuid,name,name_key) VALUES(1,'t1','Kick','kick')")
        with self.assertRaises(sqlite3.IntegrityError):
            self.db.execute("INSERT INTO tags(uuid,name,name_key) VALUES('t2','KICK','kick')")
        self.db.execute('UPDATE tags SET deleted=1 WHERE id=1')
        self.db.execute("INSERT INTO tags(uuid,name,name_key) VALUES('t2','KICK','kick')")

    def test_self_parent_rejected(self):
        with self.assertRaises(sqlite3.IntegrityError):
            self.db.execute("INSERT INTO tags(id,uuid,parent_id,name,name_key) VALUES(1,'t1',1,'a','a')")

    def test_location_live_path_uniqueness(self):
        sql="INSERT INTO locations(sample_id,root_id,relative_path,path_key,state,observed_content_revision) VALUES(1,1,'a.wav','a.wav',?,1)"
        self.db.execute(sql,('present',))
        with self.assertRaises(sqlite3.IntegrityError):
            self.db.execute(sql,('missing',))
        self.db.execute("UPDATE locations SET state='retired'")
        self.db.execute(sql,('present',))

    def test_collection_query_and_ordinal(self):
        with self.assertRaises(sqlite3.IntegrityError):
            self.db.execute("INSERT INTO collections(uuid,name,kind) VALUES('c1','smart','smart')")
        self.db.execute("INSERT INTO collections(id,uuid,name,kind) VALUES(1,'c1','manual','manual')")
        self.db.execute('INSERT INTO collection_items VALUES(1,1,0)')
        self.add_sample(2,'snare')
        with self.assertRaises(sqlite3.IntegrityError):
            self.db.execute('INSERT INTO collection_items VALUES(1,2,0)')

    def test_json_validation(self):
        with self.assertRaises(sqlite3.IntegrityError):
            self.db.execute("INSERT INTO library_settings VALUES('a',1,'not-json')")

    def test_job_state_and_per_run_idempotency(self):
        self.db.execute("INSERT INTO analysis_profiles VALUES(1,'p1',1,'waveform','{}','digest',0)")
        self.db.execute("INSERT INTO analysis_runs(id,uuid,profile_id,state,requested_at,input_revision,selection_json) VALUES(1,'r1',1,'queued',0,0,'{}')")
        stmt='INSERT INTO jobs(uuid,run_id,sample_id,content_revision,stage,state) VALUES(?,1,1,1,?,?)'
        self.db.execute(stmt,('j1','waveform','queued'))
        with self.assertRaises(sqlite3.IntegrityError):
            self.db.execute(stmt,('j2','waveform','queued'))
        with self.assertRaises(sqlite3.IntegrityError):
            self.db.execute(stmt,('j3','key','invented'))

    def test_command_receipt_identity(self):
        self.db.execute("INSERT INTO command_receipts VALUES('op1','digest','completed',NULL,'{}',0)")
        with self.assertRaises(sqlite3.IntegrityError):
            self.db.execute("INSERT INTO command_receipts VALUES('op1','different','completed',NULL,'{}',0)")
        self.assertEqual(self.db.execute("SELECT command_digest FROM command_receipts WHERE operation_uuid='op1'").fetchone()[0], 'digest')

    def test_cross_run_analysis_claim_uniqueness(self):
        self.db.execute("INSERT INTO analysis_profiles VALUES(1,'p1',1,'waveform','{}','digest',0)")
        self.db.execute("INSERT INTO analysis_runs(id,uuid,profile_id,state,requested_at,input_revision,selection_json) VALUES(1,'r1',1,'queued',0,0,'{}')")
        self.db.execute("INSERT INTO jobs(id,uuid,run_id,sample_id,content_revision,stage,state) VALUES(1,'j1',1,1,1,'waveform','queued')")
        self.db.execute("INSERT INTO analysis_claims VALUES(1,1,'stage-hash',1,'lease',100)")
        with self.assertRaises(sqlite3.IntegrityError):
            self.db.execute("INSERT INTO analysis_claims VALUES(1,1,'stage-hash',1,'other-lease',200)")

    def test_vector_active_generation_uniqueness(self):
        self.db.execute("INSERT INTO vector_spaces VALUES(1,'s','m','p',512,'cosine','{}')")
        self.db.execute("INSERT INTO index_generations VALUES(1,1,1,'a','hash',0,'active')")
        with self.assertRaises(sqlite3.IntegrityError):
            self.db.execute("INSERT INTO index_generations VALUES(2,1,2,'b','hash',1,'active')")

    def test_duplicate_pair_order(self):
        self.add_sample(2,'snare')
        stmt="INSERT INTO duplicate_edges VALUES(?,?,1,1,'near','v1',0.9,'{}',0)"
        self.db.execute(stmt,(1,2))
        with self.assertRaises(sqlite3.IntegrityError):
            self.db.execute(stmt,(2,1))

    def test_unknown_duration_distinct_from_zero(self):
        self.db.execute('INSERT INTO effective_metadata(sample_id,content_revision,duration_us) VALUES(1,1,NULL)')
        self.assertIsNone(self.db.execute('SELECT duration_us FROM effective_metadata').fetchone()[0])
        self.db.execute('UPDATE effective_metadata SET duration_us=0')
        self.assertEqual(self.db.execute('SELECT duration_us FROM effective_metadata').fetchone()[0],0)

    def test_backup_roundtrip(self):
        self.add_search(1,'kick')
        self.db.commit()
        copy=sqlite3.connect(':memory:')
        self.db.backup(copy)
        self.assertEqual(copy.execute('SELECT uuid FROM samples').fetchall(),[('sample-1',)])
        self.assertEqual(copy.execute("SELECT rowid FROM sample_fts WHERE sample_fts MATCH 'kick'").fetchall(),[(1,)])
        self.assertEqual(copy.execute('PRAGMA integrity_check').fetchone()[0],'ok')
        copy.close()

if __name__=='__main__':
    print(f'Specification checks only; Python SQLite {sqlite3.sqlite_version}', flush=True)
    unittest.main(verbosity=2)
