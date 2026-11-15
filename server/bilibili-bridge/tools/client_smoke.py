"""Bounded HTTPS device-client checks; private token is read only from a file."""
import argparse
import hashlib
import http.client
import json
from pathlib import Path
import ssl
from urllib.parse import urlsplit, quote

parser=argparse.ArgumentParser()
parser.add_argument('--config-file',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--qr',action='store_true',help='Create/poll a normal unconfirmed QR login; never prints its key or URL')
args=parser.parse_args()
config=json.loads(args.config_file.read_text('utf-8'))
base=urlsplit(config['base_url'])
assert base.scheme=='https' and base.hostname and not base.username and not base.password and not base.query and not base.fragment
token=config['device_token']
assert 24<=len(token)<=192 and all(32<ord(x)<127 for x in token)
context=ssl.create_default_context()
context.maximum_version=ssl.TLSVersion.TLSv1_2
certificate={}
def request(method,path,body=None):
    connection=http.client.HTTPSConnection(base.hostname,base.port or 443,context=context,timeout=20)
    try:
        connection.connect()
        if not certificate:
            peer=connection.sock.getpeercert()
            certificate.update(tls=connection.sock.version(),san=peer.get('subjectAltName'),
                sha256=hashlib.sha256(connection.sock.getpeercert(binary_form=True)).hexdigest(),not_after=peer.get('notAfter'))
        connection.request(method,base.path.rstrip('/')+path,body=body,
            headers={'Authorization':'Bearer '+token,'Content-Type':'application/json'})
        response=connection.getresponse();raw=response.read(65537)
        if len(raw)>65536:raise ValueError('response_limit')
        return response.status,json.loads(raw)
    finally:connection.close()
record={'base_url':config['base_url'],'phone_confirmation_tested':False,'private_values_recorded':False,'checks':{}}
try:
    status,value=request('GET','/v1/health')
    record['checks']['health']=status==200 and value.get('ok') is True
    status,value=request('GET','/v1/feed?page=1')
    items=(value.get('data') or {}).get('items',[])
    record['feed_items']=len(items);record['checks']['authenticated_feed']=status==200 and value.get('ok') is True and 0<len(items)<=10
    status,value=request('GET','/v1/me')
    record['me_status']=status;record['account_logged_in']=(value.get('data') or {}).get('logged_in')
    record['checks']['account_https_route']=status==200 and value.get('ok') is True
    if args.qr:
        status,value=request('POST','/v1/auth/qr','{}');data=value.get('data') or {}
        record['qr_create_status']=status;record['qr_url_host']=urlsplit(data.get('qr_url','')).hostname
        if status != 200: record['qr_error_code']=(value.get('error') or {}).get('code')
        record['qr_expires_in']=data.get('expires_in')
        record['checks']['qr_created']=status==200 and value.get('ok') is True and bool(data.get('login_id')) and bool(data.get('qr_url'))
        if data.get('login_id'):
            status,value=request('GET','/v1/auth/qr?login_id='+quote(data['login_id'],safe=''))
            record['qr_poll_http_status']=status;record['qr_status']=(value.get('data') or {}).get('status')
            record['checks']['qr_polled']=status==200 and value.get('ok') is True and record['qr_status'] in ['waiting','scanned','confirmed','expired']
    record['passed']=all(record['checks'].values())
except Exception as error:
    record['passed']=False;record['error_class']=type(error).__name__
record['certificate']=certificate
args.output.write_text(json.dumps(record,ensure_ascii=False,indent=2)+'\n','utf-8')
print(json.dumps(record,ensure_ascii=True))
raise SystemExit(0 if record['passed'] else 2)
