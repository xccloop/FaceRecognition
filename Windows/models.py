"""
SQLAlchemy ORM 模型定义
"""
from datetime import datetime
from sqlalchemy import Column, Integer, String, Float, Boolean, DateTime, create_engine
from sqlalchemy.orm import declarative_base, sessionmaker
from config import DATABASE_URL

Base = declarative_base()
engine = create_engine(DATABASE_URL, connect_args={"check_same_thread": False})
SessionLocal = sessionmaker(autocommit=False, autoflush=False, bind=engine)


class User(Base):
    """注册人员"""
    __tablename__ = "users"

    id = Column(Integer, primary_key=True, autoincrement=True)
    name = Column(String(50), nullable=False)
    photo_path = Column(String(500), default="")
    feature_path = Column(String(500), default="")
    pi_synced = Column(Boolean, default=False)
    created_at = Column(DateTime, default=datetime.utcnow)


class RecognitionLog(Base):
    """识别日志（树莓派回传）"""
    __tablename__ = "recognition_logs"

    id = Column(Integer, primary_key=True, autoincrement=True)
    user_id = Column(Integer, nullable=True)
    name = Column(String(50), default="")
    confidence = Column(Float, default=0.0)
    is_stranger = Column(Boolean, default=False)
    timestamp = Column(DateTime, default=datetime.utcnow)


class SystemStatus(Base):
    """系统状态（单行记录，id 始终=1）"""
    __tablename__ = "system_status"

    id = Column(Integer, primary_key=True, default=1)
    pi_online = Column(Boolean, default=False)
    pi_uptime = Column(Integer, default=0)
    camera_fps = Column(Float, default=0.0)
    last_heartbeat = Column(DateTime, nullable=True)
    updated_at = Column(DateTime, default=datetime.utcnow, onupdate=datetime.utcnow)


def init_db():
    """创建所有表"""
    Base.metadata.create_all(bind=engine)
    # 确保 system_status 有默认行
    session = SessionLocal()
    try:
        if not session.query(SystemStatus).filter(SystemStatus.id == 1).first():
            session.add(SystemStatus(id=1))
            session.commit()
    finally:
        session.close()
