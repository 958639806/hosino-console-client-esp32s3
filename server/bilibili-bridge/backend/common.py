"""Shared validation and private state; no credentials are logged."""
from __future__ import annotations

import json
import math
import os
from pathlib import Path
import re
import secrets
import stat


class ApiError(Exception):
    def __init__(self, code, message, status=400):
        self.code, self.message, self.status = code, message, status
        super().__init__(code)


def require(condition, code="invalid_request", message="请求参数不正确", status=400):
    if not condition:
        raise ApiError(code, message, status)


def positive(value, name="id", maximum=2**53 - 1):
    require(not isinstance(value, bool) and str(value).isdigit(), message=name + "必须是正整数")
    result = int(value)
    require(0 < result <= maximum, message=name + "超出范围")
    return result


def bvid(value):
    require(isinstance(value, str) and re.fullmatch(r"BV[0-9A-Za-z]{10}", value), message="BVID不正确")
    return value


def page(value):
    return positive(value, "page", 1000)


def position(value):
    require(type(value) in (int, float) and math.isfinite(value) and 0 <= value <= 86400,
            message="播放位置不正确")
    return float(value)


def private_directory(path):
    path = Path(path).resolve()
    path.mkdir(parents=True, exist_ok=True, mode=0o700)
    if os.name == "posix":
        require(stat.S_IMODE(path.stat().st_mode) & 0o077 == 0,
                "private_state_permissions", "状态目录必须为0700权限", 500)
    return path


def private_read(path):
    path = Path(path)
    require(not path.is_symlink(), "private_file_symlink", "私有文件不能为符号链接", 500)
    if os.name == "posix":
        require(stat.S_IMODE(path.stat().st_mode) & 0o077 == 0,
                "private_file_permissions", "私有文件必须为0600权限", 500)
    require(path.stat().st_size <= 65536, "private_file_size", "私有文件过大", 500)
    return path.read_text("utf-8")


def private_write(path, value):
    path = Path(path)
    require(not path.is_symlink(), "private_file_symlink", "私有文件不能为符号链接", 500)
    temporary = path.with_name(path.name + ".tmp-" + secrets.token_hex(8))
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as target:
            json.dump(value, target, ensure_ascii=False)
            target.flush()
            os.fsync(target.fileno())
        os.replace(temporary, path)
        if os.name == "posix":
            path.chmod(0o600)
    finally:
        if temporary.exists():
            temporary.unlink()
