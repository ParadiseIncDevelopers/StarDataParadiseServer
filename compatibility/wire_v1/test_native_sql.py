"""Transport boundary tests; native_fork.py supplies real SQL/TLS acceptance."""
from decimal import Decimal
from datetime import timedelta
import struct
import json
import base64
import hashlib
import unittest

from mysql.connector.constants import FieldType
from mysql.connector.protocol import MySQLProtocol
from mysql.connector.errors import ProgrammingError, ReadTimeoutError
from native_sql import NativeSession, NativeProtocol, NativeConverter, binding, cell, column, native_error
from sql import Fault


def description(name,code,charset=45): return (name,code,None,None,None,None,1,0,charset)


class Database:
    def __init__(self):
        self.in_transaction=False
        self.unread_result=False
        self._have_next_result=False
        self.read_timeout=20
        self.source=None
        self.arguments=None
        self.closed=False
        self.rows=[(("{\"id\":9007199254740993}",Decimal('123.4500'),2**63+1),)]
    def cmd_query_iter(self,source):
        self.source=source
        yield {'columns':[description('document',FieldType.JSON),description('amount',FieldType.NEWDECIMAL),description('large',FieldType.LONGLONG)],'eof':{'status_flag':2}}
    def get_rows(self,**options): return [self.rows.pop(0)[0]] if self.rows else [], {'status_flag':2}
    def cmd_stmt_prepare(self,source):
        self.source=source
        return {'statement_id':1,'parameters':[description('?',FieldType.STRING)]}
    def cmd_stmt_execute(self,statement_id,**arguments):
        self.arguments=arguments
        return {'affected_rows':1,'status_flag':2}
    def cmd_stmt_close(self,statement_id): self.statement_closed=statement_id
    def close(self): self.closed=True


class NativeTransportTests(unittest.TestCase):
    def test_binary_bindings_use_blob_protocol_type_with_empty_and_non_utf8_bytes(self):
        values=(b'\x00\xff','Türkçe',b'',None)
        for attrs in (None,[],[('trace',b'\xff')]):
            original=MySQLProtocol().make_stmt_execute(7,values,(None,)*4,query_attrs=attrs)
            actual=NativeProtocol().make_stmt_execute(7,values,(None,)*4,query_attrs=attrs)
            # All parameter values, UTF-8 text and metadata remain byte exact;
            # only the wire type of binary parameters changes.
            expected=bytearray(original)
            offset=11 if attrs is None else 12
            expected[offset]=FieldType.BLOB
            expected[offset+(4 if attrs is None else 6)]=FieldType.BLOB
            if attrs:expected[offset+12]=FieldType.BLOB
            self.assertEqual(bytes(expected),actual)

    def test_binary_flag_text_uses_result_charset_and_binary_columns_keep_raw_bytes(self):
        self.assertEqual({'t':'text','v':'İğ'},cell('İğ'.encode('utf-8'),{'type':'TEXT'}))
        self.assertEqual({'t':'binary','v':'00ff'},cell(b'\x00\xff',{'type':'BLOB'}))
        with self.assertRaises(UnicodeError):cell(b'\xff',{'type':'TEXT'})

    def test_json_bigint_and_decimal_digits_survive_transport(self):
        db=Database()
        result=NativeSession(db).execute({'sql':'SELECT document, amount, large FROM sample'})
        row=result['sets'][0]['rows'][0]
        self.assertEqual({'t':'json','v':'{"id":9007199254740993}'},row[0])
        self.assertEqual({'t':'decimal','v':'123.4500'},row[1])
        self.assertEqual({'t':'u64','v':str(2**63+1)},row[2])
        self.assertFalse(result['in_transaction'])
        self.assertTrue(result['autocommit'])

    def test_native_binding_never_interpolates_sql_or_marker_text(self):
        db=Database()
        text="'); DROP TABLE sample; -- :local ?"
        result=NativeSession(db).execute({'sql':'APPEND PROMPT ?','parameters':[{'t':'text','v':text}]})
        self.assertEqual(b'APPEND PROMPT ?',db.source)
        self.assertEqual((text,),db.arguments['data'])
        self.assertEqual('1',result['sets'][0]['affected_rows'])
        self.assertEqual(1,db.statement_closed)

    def test_procedure_body_reaches_native_parser_unchanged(self):
        db=Database()
        sql='CREATE PROCEDURE p() AS LET x INT := 1; SELECT :x; END'
        NativeSession(db).execute({'sql':sql})
        self.assertEqual(sql,db.source)

    def test_wait_timeout_preserves_committed_run_state(self):
        error=ProgrammingError(msg='511003: {"commit_state":"committed","run_id":"'+'1'*32+'","run_state":"running","retry":{"action":"QUERY_RUN"}}',errno=61093,sqlstate='HYT00')
        fault=native_error(error)
        self.assertEqual(511003,fault.document['code'])
        self.assertEqual(61093,fault.document['native_errno'])
        self.assertEqual('committed',fault.document['commit_state'])
        self.assertFalse(fault.document['unknown_outcome'])
        self.assertFalse(fault.close)

    def test_connection_loss_is_uncertain_and_never_implies_rollback(self):
        fault=native_error(ReadTimeoutError(msg='read timeout',errno=3024))
        self.assertTrue(fault.document['unknown_outcome'])
        self.assertTrue(fault.close)

    def test_routine_signal_does_not_claim_earlier_commits_rolled_back(self):
        error=ProgrammingError(msg='routine failed after commit',errno=1644,sqlstate='45000')
        self.assertTrue(native_error(error).document['unknown_outcome'])
        self.assertFalse(native_error(error,executed=False).document['unknown_outcome'])

    def test_tool_rollback_keeps_wrapper_errno_and_separate_action_outcome(self):
        details={'phase':'tool','commit_state':'committed','run_state':'running','run_id':'a'*32,
                 'call_id':'b'*32,'action_commit_state':'rolled_back','unknown_outcome':False,
                 'retry':{'action':'NONE'},'cause_native_errno':3024,'cause_sqlstate':'HY000',
                 'native_errno':3024}
        error=ProgrammingError(msg='510005: '+json.dumps(details),errno=61085,sqlstate='45000')
        result=native_error(error).document
        self.assertEqual(510005,result['code'])
        self.assertEqual(61085,result['native_errno'])
        self.assertEqual(3024,result['cause_native_errno'])
        self.assertEqual('HY000',result['cause_sqlstate'])
        self.assertEqual('b'*32,result['call_id'])
        self.assertEqual('committed',result['commit_state'])
        self.assertEqual('rolled_back',result['action_commit_state'])
        self.assertFalse(result['unknown_outcome'])

    def test_embedded_false_never_overrides_transport_or_missing_outcome_proof(self):
        for detail in ({'unknown_outcome':False},
                       {'unknown_outcome':False,'commit_state':[], 'action_commit_state':{}},
                       {'unknown_outcome':False,'commit_state':'committed','action_commit_state':'unknown'}):
            error=ProgrammingError(msg='510005: '+json.dumps(detail),errno=61085,sqlstate='45000')
            self.assertTrue(native_error(error).document['unknown_outcome'])
        lost=ProgrammingError(msg='510005: '+json.dumps({'unknown_outcome':False,
             'commit_state':'committed','action_commit_state':'rolled_back'}),errno=2013)
        result=native_error(lost)
        self.assertTrue(result.close)
        self.assertTrue(result.document['unknown_outcome'])
        self.assertEqual('unknown',result.document['action_commit_state'])

    def test_mysql_time_is_signed_total_hours(self):
        metadata={'type':'TIME'}
        self.assertEqual('-01:00:00',cell(timedelta(hours=-1),metadata)['v'])
        self.assertEqual('25:00:00',cell(timedelta(hours=25),metadata)['v'])
        self.assertEqual('-00:00:00.000001',cell(timedelta(microseconds=-1),metadata)['v'])

    def test_binary_negative_time_sign_covers_hours_and_microseconds(self):
        packet=bytes([12,1])+struct.pack('<I',0)+bytes([1,2,3])+struct.pack('<I',4)
        remaining,value=NativeProtocol._parse_binary_time(packet)
        self.assertEqual(b'',remaining)
        self.assertEqual('-01:02:03.000004',cell(value,{'type':'TIME'})['v'])

    def test_zero_dates_are_rejected_instead_of_silently_becoming_null(self):
        self.assertIsNone(NativeConverter._date_to_python(None))
        for function,value in ((NativeConverter._date_to_python,b'0000-00-00'),
                               (NativeConverter._datetime_to_python,b'0000-00-00 12:00:00')):
            with self.assertRaises(Fault): function(value)
        with self.assertRaises(Fault): NativeProtocol._parse_binary_timestamp(b'\0',FieldType.DATE)

    def test_large_json_is_chunked_without_reexecuting_or_exceeding_wire_frame(self):
        db=Database()
        document=json.dumps({'text':'İğ'*200000},ensure_ascii=False)
        db.rows=[((document,Decimal('1.0000'),2**63+1),)]
        session=NativeSession(db)
        response=session.execute({'sql':'HIST SHOW LAST'})
        payload=bytearray()
        while True:
            self.assertLess(len(json.dumps(response).encode()),1024*1024)
            chunk=response['native_result']
            self.assertEqual(len(payload),chunk['offset'])
            payload.extend(base64.b64decode(chunk['chunk']))
            if chunk['end']: break
            with self.assertRaises(Fault): session.execute({'sql':'COMMIT'})
            response=session.fetch({'result_id':chunk['id'],'offset':len(payload)})
        self.assertEqual(chunk['sha256'],hashlib.sha256(payload).hexdigest())
        self.assertEqual(document,json.loads(payload)['sets'][0]['rows'][0][0]['v'])
        self.assertEqual('HIST SHOW LAST',db.source)
        self.assertIsNone(session._pending)

    def test_invalid_binding_is_rejected_before_database_io(self):
        for value in ({'t':'i64','v':str(2**63)}, {'t':'u64','v':'-1'},
                      {'t':'float','v':'NaN'},{'t':'json','v':'{"a":NaN}'},
                      {'t':'null','v':None}):
            with self.assertRaises(Fault): binding(value)


if __name__=='__main__': unittest.main()
