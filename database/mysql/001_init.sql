-- V3 / MySQL schema 1. Apply only to a verified EMPTY, explicitly named schema.
-- DDL implicitly commits. An interrupted initialization is not rolled back.
CREATE TABLE schema_meta (
 singleton TINYINT NOT NULL PRIMARY KEY,
 application_id VARBINARY(32) NOT NULL,
 schema_version INT NOT NULL,
 initialized TINYINT NOT NULL,
 CONSTRAINT meta_singleton CHECK (singleton=1),
 CONSTRAINT meta_version CHECK (schema_version>0),
 CONSTRAINT meta_initialized CHECK (initialized IN (0,1))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;

CREATE TABLE device (
 dev_id VARBINARY(64) NOT NULL PRIMARY KEY,
 imsi VARBINARY(64) NOT NULL,
 first_seen_ms BIGINT NOT NULL,
 last_login_ms BIGINT NOT NULL,
 CONSTRAINT device_id_length CHECK (OCTET_LENGTH(dev_id) BETWEEN 1 AND 64),
 CONSTRAINT device_imsi_length CHECK (OCTET_LENGTH(imsi) BETWEEN 1 AND 64)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;

CREATE TABLE alarm (
 id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,
 dev_id VARBINARY(64) NOT NULL,
 event_no BIGINT NOT NULL,
 alarm_id VARBINARY(64) NOT NULL,
 alarm_desc VARBINARY(64) NOT NULL,
 status VARBINARY(16) NOT NULL,
 first_ms BIGINT NOT NULL,
 last_ms BIGINT NOT NULL,
 clear_ms BIGINT NULL,
 open_guard TINYINT GENERATED ALWAYS AS (CASE WHEN status=_binary'open' THEN 1 ELSE NULL END) STORED,
 CONSTRAINT alarm_device_fk FOREIGN KEY (dev_id) REFERENCES device(dev_id),
 CONSTRAINT alarm_event_positive CHECK (event_no>0),
 CONSTRAINT alarm_status CHECK (status IN (_binary'open',_binary'cleared')),
 CONSTRAINT alarm_lifecycle CHECK ((status=_binary'open' AND clear_ms IS NULL) OR (status=_binary'cleared' AND clear_ms IS NOT NULL)),
 UNIQUE KEY alarm_event (dev_id,event_no),
 UNIQUE KEY one_open_alarm (dev_id,alarm_id,alarm_desc,open_guard),
 KEY alarm_device_id (dev_id,id),
 KEY alarm_device_status_id (dev_id,status,id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;

CREATE TABLE query_task (
 txn_no BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,
 dev_id VARBINARY(64) NOT NULL,
 connection_id VARBINARY(128) NULL,
 status VARBINARY(16) NOT NULL,
 reason VARBINARY(128) NULL,
 created_ms BIGINT NOT NULL,
 finished_ms BIGINT NULL,
 response_json LONGTEXT CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NULL,
 client_request_id VARBINARY(64) NULL,
 pending_guard TINYINT GENERATED ALWAYS AS (CASE WHEN status=_binary'pending' THEN 1 ELSE NULL END) STORED,
 CONSTRAINT query_device_fk FOREIGN KEY (dev_id) REFERENCES device(dev_id),
 CONSTRAINT query_status CHECK (status IN (_binary'pending',_binary'success',_binary'timeout',_binary'link_lost')),
 CONSTRAINT query_lifecycle CHECK ((status=_binary'pending' AND finished_ms IS NULL) OR (status<>_binary'pending' AND finished_ms IS NOT NULL)),
 CONSTRAINT query_response_json CHECK (response_json IS NULL OR JSON_VALID(response_json)),
 CONSTRAINT query_request_length CHECK (client_request_id IS NULL OR OCTET_LENGTH(client_request_id) BETWEEN 1 AND 64),
 UNIQUE KEY one_pending_query (dev_id,pending_guard),
 UNIQUE KEY unique_client_request (client_request_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;

-- The initializer validates the manifest before executing this final statement.
INSERT INTO schema_meta(singleton,application_id,schema_version,initialized) VALUES(1,_binary'batt-access',1,1);
