from __future__ import annotations

import getpass
import sys

from app import (
    MIN_PASSWORD_LENGTH,
    database,
    initialize_database,
    iso_time,
    password_hasher,
    utc_now,
)


def main() -> int:
    initialize_database()
    if len(sys.argv) > 1:
        print("密码不能通过命令行参数传入；请使用交互输入或 stdin", file=sys.stderr)
        return 2
    if sys.stdin.isatty():
        password = getpass.getpass("新的管理员密码: ")
    else:
        password = sys.stdin.readline().rstrip("\r\n")
    if len(password) < MIN_PASSWORD_LENGTH:
        print(f"管理员密码必须至少为 {MIN_PASSWORD_LENGTH} 个字符", file=sys.stderr)
        return 2
    with database() as db:
        admin = db.execute("SELECT id, username FROM admins ORDER BY id LIMIT 1").fetchone()
        if admin is None:
            print("管理员账户不存在", file=sys.stderr)
            return 3
        db.execute(
            "UPDATE admins SET password_hash=?, updated_at=? WHERE id=?",
            (password_hasher.hash(password), iso_time(utc_now()), admin["id"]),
        )
        db.execute("DELETE FROM sessions WHERE subject_type='admin'")
    print(f"管理员 {admin['username']} 的密码已重置，现有管理会话已失效")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
