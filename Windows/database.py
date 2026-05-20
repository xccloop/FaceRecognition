"""
数据库会话依赖注入
"""
from models import SessionLocal


def get_db():
    """FastAPI 依赖：获取数据库会话，请求结束后自动关闭"""
    db = SessionLocal()
    try:
        yield db
    finally:
        db.close()
