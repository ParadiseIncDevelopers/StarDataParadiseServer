"""Versioned native SQL transport, separate from wire-v1's durable CRUD API.

SQL is compiled by MySQL. There is no gateway SQL classifier or named-marker
rewriter here. Bound statements use the native prepared-statement protocol;
statements without bindings use one COM_QUERY with MULTI_STATEMENTS disabled.
"""
from datetime import date, datetime, timedelta
from decimal import Decimal
import json
import logging
import math
import re
import struct
import base64
import hashlib
import secrets
import traceback
from pathlib import Path

from mysql.connector.constants import FieldType, ServerFlag
from mysql.connector.errors import Error, ReadTimeoutError, WriteTimeoutError
from mysql.connector.conversion import MySQLConverter
from mysql.connector.protocol import MySQLProtocol

from sql import Fault, require

MAX_FRAME = 1024 * 1024
MAX_RESULT = 16 * 1024 * 1024


class NativeConverter(MySQLConverter):
    @staticmethod
    def _date_to_python(value,dsc=None):
        if value is None: return None
        result=MySQLConverter._date_to_python(value,dsc)
        if value is not None and result is None: raise Fault(1004)
        return result

    @staticmethod
    def _datetime_to_python(value,dsc=None):
        if value is None: return None
        result=MySQLConverter._datetime_to_python(value,dsc)
        if value is not None and result is None: raise Fault(1004)
        return result

    _NEWDATE_to_python=_date_to_python
    _timestamp_to_python=_datetime_to_python


class NativeProtocol(MySQLProtocol):
    def make_stmt_execute(self, statement_id, data=(), parameters=(), flags=0,
                          long_data_used=None, charset='utf8', query_attrs=None,
                          converter_str_fallback=False):
        packet=super().make_stmt_execute(statement_id,data,parameters,flags,
                    long_data_used,charset,query_attrs,converter_str_fallback)
        values=tuple(data)+tuple(value for _,value in (query_attrs or ()))
        if not any(isinstance(value,bytes) for value in values): return packet
        # The pinned connector sends both str and bytes as MYSQL_TYPE_STRING.
        # Preserve the caller's binary tag in COM_STMT_EXECUTE itself; changing
        # SQL text or guessing binary data from a result value would be wrong.
        packet=bytearray(packet)
        def length_at(offset):
            first=packet[offset]
            width={252:2,253:3,254:8}.get(first,0)
            if first<251:return offset+1,first
            if not width:raise ValueError('Invalid prepared metadata length.')
            return offset+1+width,int.from_bytes(packet[offset+1:offset+1+width],'little')
        offset=9  # statement ID, flags and iteration count
        if query_attrs is not None: offset,_=length_at(offset)
        offset+=(len(values)+7)//8+1  # null bitmap and new-parameter flag
        for index,value in enumerate(values):
            if isinstance(value,bytes) and index not in (long_data_used or {}):
                packet[offset]=FieldType.BLOB
            offset+=2
            if query_attrs is not None:
                offset,length=length_at(offset)
                offset+=length
        return bytes(packet)

    @staticmethod
    def _parse_binary_timestamp(packet,field_type):
        # A zero date is distinct from SQL NULL's bitmap bit. The standard
        # connector maps both to None; reject unrepresentable dates explicitly.
        if packet[0]==0: raise Fault(1004)
        try: return MySQLProtocol._parse_binary_timestamp(packet,field_type)
        except ValueError: raise Fault(1004) from None

    @staticmethod
    def _parse_binary_time(packet):
        length=packet[0]
        if length==0: return packet[1:],timedelta()
        if length not in (8,12) or len(packet)<length+1: raise Fault(1004)
        data=packet[1:length+1]
        days=struct.unpack('<I',data[1:5])[0]
        micros=struct.unpack('<I',data[8:12])[0] if length==12 else 0
        value=timedelta(days=days,hours=data[5],minutes=data[6],seconds=data[7],microseconds=micros)
        if data[0]: value=-value
        return packet[length+1:],value


class NativeFault(Fault):
    def __init__(self, document, *, close=False):
        super().__init__(document['code'], close=close, uncertain=document.get('unknown_outcome', False))
        self.document = document


def native_error(error, *, executed=True):
    lost = isinstance(error,(ReadTimeoutError,WriteTimeoutError)) or error.errno in (2006,2013,2055)
    text = str(getattr(error,'msg','') or 'Native SQL failed.')[:4096]
    code = int(error.errno or 1015)
    extra = {}
    prefix = re.match(r'^([1-9][0-9]{3,5}):\s*',text)
    if prefix:
        code = int(prefix[1])
        try:
            details = json.loads(text[prefix.end():])
            if isinstance(details,dict):
                for key in ('phase','run_state'):
                    value=details.get(key)
                    if isinstance(value,str) and re.fullmatch(r'[a-z_]{1,32}',value): extra[key]=value
                for key in ('run_id','call_id'):
                    value=details.get(key)
                    if isinstance(value,str) and re.fullmatch('[0-9a-f]{32}',value): extra[key]=value
                for key,allowed in (('commit_state',{'committed','not_committed','unknown'}),
                                    ('action_commit_state',{'not_started','rolled_back','committed','unknown'})):
                    value=details.get(key)
                    if isinstance(value,str) and value in allowed: extra[key]=value
                if type(details.get('unknown_outcome')) is bool: extra['unknown_outcome']=details['unknown_outcome']
                retry=details.get('retry')
                if isinstance(retry,dict) and set(retry)=={'action'} and isinstance(retry['action'],str) and re.fullmatch('[A-Z_]{1,32}',retry['action']):
                    extra['retry']=retry
                cause=details.get('cause_native_errno')
                if type(cause) is int and 1<=cause<=65535: extra['cause_native_errno']=cause
                state=details.get('cause_sqlstate')
                if isinstance(state,str) and re.fullmatch('[A-Z0-9]{5}',state): extra['cause_sqlstate']=state
        except (ValueError,RecursionError):
            pass
    # A native routine can COMMIT and subsequently SIGNAL. A statement error
    # alone therefore cannot prove that all its earlier effects rolled back.
    unknown=lost or extra.get('unknown_outcome') is True or extra.get('action_commit_state')=='unknown' or \
        (executed and error.errno not in (1064,1295) and extra.get('commit_state') not in ('committed','not_committed'))
    # An embedded false flag cannot override transport uncertainty or establish
    # a rollback by itself. The parent AI turn and the tool transaction have
    # independent outcomes; keep the native wrapper's errno separate from its cause.
    extra.pop('unknown_outcome',None)
    if lost and 'action_commit_state' in extra: extra['action_commit_state']='unknown'
    document = {'code':code,'native_errno':error.errno,'sqlstate':error.sqlstate or 'HY000',
                'message':text,'line':None,'column':None,'retryable':False,
                'unknown_outcome':unknown,'commit_state':'unknown' if unknown else 'not_committed',
                'correlation_id':None,**extra}
    return NativeFault(document,close=lost)


def limit_fault():
    # A procedure may have committed before its result exceeded the transport
    # budget. Never describe this as a known rollback or retry it automatically.
    return NativeFault({'code':1014,'sqlstate':'53000','message':'Native SQL result exceeds the bounded response budget.',
                        'line':None,'column':None,'retryable':False,'unknown_outcome':True,
                        'commit_state':'unknown','correlation_id':None},close=True)


def binding(value):
    require(isinstance(value,dict))
    tag = value.get('t')
    if tag=='null':
        require(set(value)=={'t'})
        return None
    require(set(value)=={'t','v'})
    raw=value['v']
    if tag=='text':
        require(isinstance(raw,str))
        return raw
    if tag in ('i64','u64'):
        require(isinstance(raw,str) and re.fullmatch(r'0|-?[1-9][0-9]{0,19}',raw))
        number=int(raw)
        require(-(1<<63)<=number<(1<<63) if tag=='i64' else 0<=number<(1<<64))
        return number
    if tag=='bool':
        require(type(raw) is bool)
        return int(raw)
    if tag=='binary':
        require(isinstance(raw,str) and len(raw)%2==0 and re.fullmatch('[0-9a-f]*',raw))
        return bytes.fromhex(raw)
    if tag=='decimal':
        require(isinstance(raw,str) and re.fullmatch(r'-?(?:0|[1-9][0-9]{0,64})(?:\.[0-9]{1,30})?',raw))
        result=Decimal(raw)
        require(len(result.as_tuple().digits)<=65)
        return result
    if tag=='float':
        require(isinstance(raw,str))
        try: result=float(raw)
        except ValueError: raise Fault(1012) from None
        require(math.isfinite(result))
        return result
    if tag=='json':
        # A serialized JSON string preserves all integer digits across Node,
        # Java, Python and .NET. The SQL binder still validates the target type.
        require(isinstance(raw,str))
        try: json.loads(raw,parse_constant=lambda _: (_ for _ in ()).throw(ValueError()))
        except (ValueError,RecursionError): raise Fault(1012) from None
        return raw
    raise Fault(1004)


def column(column):
    code=column[1]
    names={FieldType.JSON:'JSON',FieldType.DECIMAL:'DECIMAL',FieldType.NEWDECIMAL:'DECIMAL',
           FieldType.FLOAT:'FLOAT',FieldType.DOUBLE:'DOUBLE',FieldType.DATE:'DATE',
           FieldType.DATETIME:'DATETIME',FieldType.TIMESTAMP:'TIMESTAMP',FieldType.TIME:'TIME'}
    kind=names.get(code)
    if kind is None:
        if code in (FieldType.TINY,FieldType.SHORT,FieldType.INT24,FieldType.LONG,FieldType.LONGLONG,FieldType.YEAR): kind='BIGINT'
        elif code==FieldType.NULL: kind='NULL'
        else: kind='BLOB' if len(column)>8 and column[8]==63 else 'TEXT'
    return {'name':column[0],'type':kind,'native_type':code,'nullable':bool(column[6])}


def cell(value,metadata):
    if value is None: return {'t':'null'}
    kind=metadata['type']
    if kind=='JSON':
        if isinstance(value,bytes): value=value.decode('utf-8')
        require(isinstance(value,str),1004)
        return {'t':'json','v':value}
    if kind=='TEXT' and isinstance(value,bytes):
        # Connector/Python may retain bytes for a BINARY_FLAG VARCHAR even
        # when its result charset is UTF-8. Protocol metadata distinguishes
        # text from charset-63 BLOB; preserve that declared SQL type.
        return {'t':'text','v':value.decode('utf-8',errors='strict')}
    if type(value) is int: return {'t':'i64' if value<(1<<63) else 'u64','v':str(value)}
    if isinstance(value,Decimal): return {'t':'decimal','v':format(value,'f')}
    if type(value) is float:
        require(math.isfinite(value),1004)
        return {'t':'float','v':repr(value)}
    if isinstance(value,bytes): return {'t':'binary','v':value.hex()}
    if isinstance(value,datetime): return {'t':'datetime','v':value.isoformat(' ')}
    if isinstance(value,date): return {'t':'date','v':value.isoformat()}
    if isinstance(value,timedelta):
        total=(value.days*86400+value.seconds)*1000000+value.microseconds
        sign='-' if total<0 else ''
        whole,fraction=divmod(abs(total),1000000)
        hours,rest=divmod(whole,3600)
        minutes,seconds=divmod(rest,60)
        return {'t':'time','v':f'{sign}{hours:02d}:{minutes:02d}:{seconds:02d}'+(f'.{fraction:06d}' if fraction else '')}
    require(isinstance(value,str),1004)
    return {'t':'text','v':value}


class NativeSession:
    def __init__(self,db,autocommit=True):
        self.db,self.autocommit=db,autocommit
        self._pending=None
        # Dedicated native sessions can install exact temporal adapters without
        # altering the existing CRUD bridge's value/receipt contract.
        if hasattr(db,'set_converter_class'):
            db.set_converter_class(NativeConverter)
            db._protocol=NativeProtocol()

    def _observe_status(self,packet):
        flags=packet.get('status_flag',packet.get('server_status')) if isinstance(packet,dict) else None
        if flags is not None: self.autocommit=bool(flags & ServerFlag.STATUS_AUTOCOMMIT)

    def execute(self,body):
        require(self._pending is None,1013)
        statement=body.get('sql')
        parameters=body.get('parameters',[])
        require(isinstance(statement,str) and '\0' not in statement)
        try: size=len(statement.encode('utf-8'))
        except UnicodeError: raise Fault(1012) from None
        require(0<size<=65536)
        require(isinstance(parameters,list) and len(parameters)<=128)
        values=tuple(binding(value) for value in parameters)
        try:
            for value in values:
                if isinstance(value,str): value.encode('utf-8')
        except UnicodeError: raise Fault(1012) from None
        timeout=body.get('timeout_ms',65000)
        require(type(timeout) is int and 1000<=timeout<=300000)
        row_limit=body.get('max_rows',1000)
        require(type(row_limit) is int and 1<=row_limit<=10000)
        self.db.read_timeout=(timeout+999)//1000
        output={'sets':[],'in_transaction':False,'autocommit':True}
        budget=MAX_RESULT-1024
        used_rows=0
        native=None
        executed=False
        try:
            if values:
                native=self.db.cmd_stmt_prepare(statement.encode('utf-8'))
                require(len(native['parameters'])==len(values))
                executed=True
                first=self.db.cmd_stmt_execute(native['statement_id'],data=values,parameters=native['parameters'])
                responses=self._prepared_results(first)
            else:
                # This public API passes the exact source directly to the native
                # parser. It does not split CREATE PROCEDURE bodies or comments.
                executed=True
                responses=self.db.cmd_query_iter(statement)
            for response in responses:
                self._observe_status(response if isinstance(response,dict) else response[2])
                if len(output['sets'])>=32: raise limit_fault()
                if isinstance(response,dict) and 'columns' not in response:
                    part={'columns':[],'rows':[],'affected_rows':str(response.get('affected_rows',0)),
                          'last_insert_id':str(response.get('insert_id',0))}
                else:
                    columns=response['columns'] if isinstance(response,dict) else response[1]
                    if native is not None: self.db.unread_result=True
                    if len(columns)>128: raise limit_fault()
                    metadata=[column(item) for item in columns]
                    part={'columns':metadata,'rows':[],'affected_rows':'0'}
                    while True:
                        rows,eof=self.db.get_rows(count=1,binary=native is not None,columns=columns)
                        for row in rows:
                            used_rows+=1
                            if used_rows>row_limit: raise limit_fault()
                            encoded=[cell(value,description) for value,description in zip(row,metadata,strict=True)]
                            size=len(json.dumps(encoded,ensure_ascii=False,separators=(',',':')).encode('utf-8'))+1
                            if size>budget: raise limit_fault()
                            budget-=size
                            part['rows'].append(encoded)
                        if eof is not None:
                            self._observe_status(eof)
                            break
                size=len(json.dumps({**part,'rows':[]},ensure_ascii=False,separators=(',',':')).encode('utf-8'))
                if size>budget: raise limit_fault()
                budget-=size
                output['sets'].append(part)
            output['in_transaction']=self.db.in_transaction
            output['autocommit']=self.autocommit
            encoded=json.dumps(output,ensure_ascii=False,allow_nan=False,separators=(',',':')).encode('utf-8')
            if len(encoded)>MAX_RESULT: raise limit_fault()
            if len(encoded)>MAX_FRAME//2:
                token=secrets.token_hex(16)
                self._pending=(token,encoded,hashlib.sha256(encoded).hexdigest())
                return self.fetch({'result_id':token,'offset':0})
            return output
        except Error as error:
            raise native_error(error,executed=executed) from None
        except Fault as failure:
            # A partially consumed protocol cannot be used for another command.
            if self.db.unread_result or self.db._have_next_result: self.db.close()
            if executed and not isinstance(failure,NativeFault):
                raise NativeFault({'code':failure.code,'sqlstate':'0A000' if failure.code==1004 else 'HY000',
                    'message':'Native result could not be represented. Query the operation outcome before retrying.',
                    'line':None,'column':None,'retryable':False,'unknown_outcome':True,
                    'commit_state':'unknown','correlation_id':None},close=True) from None
            raise
        except (UnicodeError,ValueError,TypeError,OverflowError) as failure:
            # Keep diagnostics useful without logging SQL, bound data, provider
            # credentials or exception messages that can contain row values.
            locations=[Path(frame.filename).name+':'+str(frame.lineno)
                       for frame in traceback.extract_tb(failure.__traceback__)[-6:]]
            logging.getLogger(__name__).error('Native SQL conversion failed: type=%s locations=%s',
                                             type(failure).__name__,locations)
            descriptors=locals().get('columns',())
            logging.getLogger(__name__).error('Native SQL result types: %s',
                [(entry[1],entry[7],entry[8]) for entry in descriptors if len(entry)>8][:128])
            self.db.close()
            raise NativeFault({'code':1015,'sqlstate':'XX000','message':'Invalid native SQL response.',
                'line':None,'column':None,'retryable':False,'unknown_outcome':executed,
                'commit_state':'unknown' if executed else 'not_committed','correlation_id':None},close=True) from None
        finally:
            if native is not None and not self.db.unread_result and not self.db._have_next_result:
                try: self.db.cmd_stmt_close(native['statement_id'])
                except Error: self.db.close()

    def fetch(self,body):
        require(self._pending is not None,1013)
        token,payload,digest=self._pending
        offset=body.get('offset')
        require(body.get('result_id')==token and type(offset) is int and 0<=offset<len(payload))
        # Base64 carries exact UTF-8 bytes, even if a boundary splits a Unicode
        # character. Every envelope stays below wire-v1's immutable 1MiB limit.
        chunk=payload[offset:offset+256*1024]
        end=offset+len(chunk)==len(payload)
        result={'native_result':{'id':token,'offset':offset,'total_bytes':len(payload),'sha256':digest,
                 'chunk':base64.b64encode(chunk).decode('ascii'),'end':end}}
        if end: self._pending=None
        return result

    def _prepared_results(self,first):
        # Connector/Python 26.10's public prepared cursor inherits the text
        # nextset decoder. Keep the binary packet adapter in this one place;
        # CALL and OUT sets are covered by the real native transport acceptance.
        yield first
        while self.db._have_next_result:
            self.db.handle_unread_result()
            yield self.db._handle_binary_result(self.db._socket.recv(read_timeout=self.db.read_timeout),self.db.read_timeout)
