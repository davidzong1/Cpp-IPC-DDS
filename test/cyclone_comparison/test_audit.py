import csv
from pathlib import Path
import tempfile
import unittest
from run import audit_raw, stats

class AuditTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.folder=Path(self.tmp.name)
        self.config={'seconds':2,'rate':1,'subscribers':1,'bytes':64}
        self.result={'publisher':{'window_end_ns':500,'rejected':0,'late_periods':0},'receivers':[{'invalid':0}]}
        self.pubs=[dict(publisher_id=0,sequence=i,start_ns=i*100,elapsed_ns=5,api_ns=4,success=1,bytes=144,payload_bytes=64,schedule_late_ns=0) for i in (2,3)]
        self.subs=[dict(publisher_id=0,sequence=i,read_ns=i*100+10,elapsed_ns=10,bytes=256,payload_bytes=64) for i in (2,3)]
    def audit(self):
        outputs=[(f'pub{i}.csv',[r for r in self.pubs if r['publisher_id']==i]) for i in range(self.config.get('publishers',1))]
        for name,data in outputs+[('sub0.csv',self.subs)]:
            with (self.folder/name).open('w') as f:
                w=csv.DictWriter(f,fieldnames=data[0]); w.writeheader(); w.writerows(data)
        return audit_raw(self.folder,self.config,self.result)
    def test_capacity_is_not_payload(self):
        self.assertEqual(self.audit()['errors'],[])
    def test_duplicate_replacing_missing(self):
        self.subs[1]=self.subs[0].copy()
        self.assertTrue(self.audit()['errors'])
    def test_wrong_time_join(self):
        self.subs[0]['elapsed_ns']=11
        self.assertTrue(self.audit()['errors'])
    def test_bad_payload_is_failure(self):
        self.result['receivers'][0]['invalid']=1
        self.assertTrue(self.audit()['errors'])
    def test_late_send_not_clean(self):
        self.result['publisher']['late_periods']=1
        self.assertTrue(self.audit()['errors'])
    def test_nearest_rank(self):
        self.assertEqual(stats(list(range(1,101)))['p99_us'],.099)
    def test_publisher_identity_is_not_sequence(self):
        self.subs[1]['publisher_id']=1
        self.assertTrue(self.audit()['errors'])
    def test_two_publishers_same_sequence_and_overlap(self):
        self.config['publishers']=2
        self.pubs += [dict(r,publisher_id=1) for r in self.pubs]
        self.subs += [dict(r,publisher_id=1) for r in self.subs]
        result=self.audit()
        self.assertEqual(result['errors'],[])
        self.assertEqual(result['concurrent_publish_max'],2)
        self.assertEqual(result['overlapped_publications'],4)

if __name__=='__main__': unittest.main()
