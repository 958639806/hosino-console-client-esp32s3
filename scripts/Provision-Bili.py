#!/usr/bin/env python3
"""Provision the Bili bridge connection over physical USB without resetting it.

The JSON file contains base_url and device_token. Neither is accepted as a CLI
argument or printed. No Wi-Fi server, bootloader action, DTR/RTS pulse or erase.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import re
import secrets
import stat
import sys
import time

ACK_OK=b"BILI_CONFIG_OK"
ACK_ERRORS={b"BILI_CONFIG_ERROR_INVALID",b"BILI_CONFIG_ERROR_SIZE",b"BILI_CONFIG_ERROR_STORAGE"}

def load_config(path):
    path=Path(path)
    if path.is_symlink() or not path.is_file() or path.stat().st_size>4096:
        raise ValueError("配置文件不存在、过大或是符号链接")
    if os.name=="posix" and stat.S_IMODE(path.stat().st_mode)&0o077:
        raise ValueError("配置文件权限必须为0600；请先限制文件权限")
    try:
        value=json.loads(path.read_text("utf-8"))
    except Exception:
        raise ValueError("配置文件必须为UTF-8 JSON对象") from None
    if not isinstance(value,dict) or set(value)!={"base_url","device_token"}:
        raise ValueError("配置文件只能包含base_url和device_token")
    base,token=value["base_url"],value["device_token"]
    if not isinstance(base,str) or not isinstance(token,str):
        raise ValueError("连接地址和设备令牌必须为字符串")
    base=base.rstrip("/")
    if len(base)>240 or not base.startswith("https://") or not base.isascii():
        raise ValueError("连接地址需要有效的HTTPS基址")
    authority,separator,path_part=base[8:].partition("/")
    if not re.fullmatch(r"[A-Za-z0-9.:-]+",authority) or any(c in base for c in "?#@%\\"):
        raise ValueError("连接地址不能包含凭据、查询、转义或特殊路径")
    if separator and (not re.fullmatch(r"[A-Za-z0-9_./-]*",path_part)
                      or any(p in (".","..") for p in path_part.split("/"))):
        raise ValueError("连接地址路径不受支持")
    if not 24<=len(token)<=192 or any(not 32<ord(c)<127 for c in token):
        raise ValueError("设备令牌需要24至192个无空白ASCII字符")
    return {"base_url":base,"device_token":token}

def command(config,request_id=None):
    request_id=request_id or secrets.token_hex(8)
    if not re.fullmatch(r"[0-9a-fA-F]{16}",request_id):raise ValueError("配置请求编号无效")
    payload=b"BILI_CONFIG "+json.dumps({**config,"request_id":request_id},ensure_ascii=True,separators=(",",":")).encode("ascii")+b"\n"
    if len(payload)>=768:
        raise ValueError("配置命令超过设备长度上限")
    return payload

def matching_ack(line,request_id,sync=False):
    parts=line.strip().split(b" ")
    if len(parts)!=2 or parts[1]!=request_id.encode("ascii"):return None
    if sync:return "ok" if parts[0]==b"BILI_SYNC_OK" else None
    if parts[0]==ACK_OK:return "ok"
    return parts[0].decode("ascii") if parts[0] in ACK_ERRORS else None

def await_ack(device,request_id,timeout,sync=False):
    deadline=time.monotonic()+timeout
    line=bytearray();dropping=False
    while time.monotonic()<deadline:
        for byte in device.read(256):
            if byte==10:
                result=matching_ack(bytes(line),request_id,sync) if not dropping else None
                if result=="ok":return
                if result:raise RuntimeError("设备拒绝配置："+result)
                line.clear();dropping=False
            elif not dropping:
                if len(line)>=512:line.clear();dropping=True
                else:line.append(byte)
    raise TimeoutError("等待设备确认超时；未收到匹配编号的成功确认，请勿视为已配置")

def provision(port,payload,timeout=20):
    import serial
    device=serial.Serial(port=None,baudrate=115200,timeout=0.2,write_timeout=3)
    # Set desired control-line states before opening. Never pulse or reset them.
    device.dtr=False;device.rts=False;device.port=port
    device.open()
    try:
        device.reset_input_buffer()  # Discard old ACKs; this does not reset the device.
        request_id=json.loads(payload[len(b"BILI_CONFIG "):])["request_id"]
        # Newline terminates stale partial input. Matching SYNC proves a parser
        # boundary and drains old ACKs before the credential-bearing command.
        device.write(b"\nBILI_SYNC "+request_id.encode("ascii")+b"\n");device.flush()
        await_ack(device,request_id,timeout,sync=True)
        device.write(payload);device.flush()
        await_ack(device,request_id,timeout)
    finally:
        device.close()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port",required=True,help="例如COM9，设备须已正常启动")
    parser.add_argument("--config-file",type=Path,required=True,help="包含连接地址及设备令牌的私有JSON文件")
    args=parser.parse_args()
    try:
        config=load_config(args.config_file)
        payload=command(config)
        provision(args.port,payload)
    except (ValueError,RuntimeError,TimeoutError) as exc:
        print(str(exc),file=sys.stderr);return 2
    except ImportError:
        print("缺少pyserial，请在已有ESP开发Python环境中运行。",file=sys.stderr);return 2
    except Exception:
        print("USB配置失败，请确认串口未被日志工具占用，且设备已正常启动。",file=sys.stderr);return 2
    print("设备已确认配置保存成功。请关闭并重新打开「哔哩哔哩」应用。")
    return 0

if __name__=="__main__":raise SystemExit(main())
