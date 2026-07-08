-- ═══════════════════════════════════════════════════════
--  WeMeet 数据库初始化脚本
--  执行: mysql -u root -p < scripts/init_db.sql
-- ═══════════════════════════════════════════════════════

CREATE DATABASE IF NOT EXISTS wemeet
    DEFAULT CHARACTER SET utf8mb4
    DEFAULT COLLATE utf8mb4_unicode_ci;

USE wemeet;

-- ── 用户表 ──────────────────────────────────────────────
-- 技术点: email 唯一索引加速登录查询, status 索引用于在线状态筛选
CREATE TABLE IF NOT EXISTS users (
    id             BIGINT PRIMARY KEY AUTO_INCREMENT,
    email          VARCHAR(128)  NOT NULL UNIQUE,
    password_hash  VARCHAR(256)  NOT NULL COMMENT 'SHA256 哈希值',
    nickname       VARCHAR(64)   NOT NULL,
    avatar_url     VARCHAR(512)  DEFAULT NULL,
    status         TINYINT       DEFAULT 0 COMMENT '0:离线 1:在线',
    created_at     TIMESTAMP     DEFAULT CURRENT_TIMESTAMP,
    updated_at     TIMESTAMP     DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_email (email),
    INDEX idx_status (status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='用户表';

-- ── 好友关系表 ──────────────────────────────────────────
-- 技术点: 复合唯一索引保证双向关系唯一, 外键级联删除
-- 事务隔离: 添加好友时使用 REPEATABLE READ + 双向插入事务
CREATE TABLE IF NOT EXISTS friendships (
    id         BIGINT PRIMARY KEY AUTO_INCREMENT,
    user_id    BIGINT NOT NULL,
    friend_id  BIGINT NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    UNIQUE KEY uk_user_friend (user_id, friend_id),
    INDEX idx_user (user_id),
    INDEX idx_friend (friend_id),
    FOREIGN KEY (user_id)   REFERENCES users(id) ON DELETE CASCADE,
    FOREIGN KEY (friend_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='好友关系表';

-- ── 会议记录表 ──────────────────────────────────────────
-- 技术点: room_id 唯一索引加速查找, 复合索引优化历史查询
CREATE TABLE IF NOT EXISTS meetings (
    id                BIGINT PRIMARY KEY AUTO_INCREMENT,
    room_id           VARCHAR(64)  NOT NULL UNIQUE,
    creator_id        BIGINT       NOT NULL,
    title             VARCHAR(256) DEFAULT '',
    start_time        TIMESTAMP    DEFAULT CURRENT_TIMESTAMP,
    end_time          TIMESTAMP    NULL,
    max_participants  INT          DEFAULT 50,
    record_path       VARCHAR(512) DEFAULT NULL,
    status            TINYINT      DEFAULT 0 COMMENT '0:进行中 1:已结束',
    INDEX idx_room (room_id),
    INDEX idx_creator_time (creator_id, start_time),
    FOREIGN KEY (creator_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='会议记录表';

-- ── 聊天消息表 ──────────────────────────────────────────
-- 技术点: 复合索引按时间排序, 可用于分页历史消息查询
CREATE TABLE IF NOT EXISTS chat_messages (
    id         BIGINT PRIMARY KEY AUTO_INCREMENT,
    room_id    VARCHAR(64)  NOT NULL,
    user_id    BIGINT       NOT NULL,
    content    TEXT         NOT NULL,
    created_at TIMESTAMP    DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_room_time (room_id, created_at),
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='聊天消息表';

-- ── 测试数据 ─────────────────────────────────────────────
-- 密码均为 123456 的 SHA256 哈希
INSERT IGNORE INTO users (email, password_hash, nickname) VALUES
    ('alice@wemeet.com', SHA2('123456', 256), 'Alice'),
    ('bob@wemeet.com',   SHA2('123456', 256), 'Bob'),
    ('carol@wemeet.com', SHA2('123456', 256), 'Carol');

-- 建立好友关系
INSERT IGNORE INTO friendships (user_id, friend_id) VALUES (1, 2), (2, 1), (1, 3), (3, 1);
