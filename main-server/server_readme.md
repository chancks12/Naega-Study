# StudySync Main Server — 구현 명세

> 작성일: 2026-05-07
> 담당: 김혜윤
> 위치: `main-server/`
> 빌드 산출물: `main-server/app/studysync_main_server`

학습 집중도/자세 분석 서비스의 **백엔드 API + DB 게이트키퍼**. 클라이언트 (MFC) 의 인증/세션/로그 인입을 처리하고 학습 서버와 모델 채널을 유지한다.

---

## 1. 큰 그림

```
┌──────────────────────────────────────────────────────────────────┐
│ 클라이언트 (MFC)                                                   │
│   AuthApi / SessionApi / WinHttpClient → HTTP                    │
│   JsonlBatchUploader → NDJSON POST                               │
└────────────────┬─────────────────────────────────────────────────┘
                 │ HTTP :8081 (JWT)
                 ▼
   ┌──────────────────────────────────────────────────────┐
   │ Main Server (C++)                                     │
   │  ┌────────────────────────────────────────────────┐  │
   │  │ HTTP layer (cpp-httplib)                       │  │
   │  │   13 endpoints (auth/goal/session/log/stats)   │  │
   │  │   JWT middleware (HS256)                       │  │
   │  │   NDJSON 트랜잭션 인입 (/log/ingest)           │  │
   │  └────────────────────────────────────────────────┘  │
   │  ┌────────────────────────────────────────────────┐  │
   │  │ TCP layer (raw socket :9000)                   │  │
   │  │   학습 채널 (TRAIN_PROGRESS / COMPLETE / FAIL) │  │
   │  │   AI 도메인 1000~1007 (선택적 사용)            │  │
   │  └────────────────────────────────────────────────┘  │
   │  ┌────────────────────────────────────────────────┐  │
   │  │ Service / DAO / EventBus                       │  │
   │  └────────────────────────────────────────────────┘  │
   │  ┌────────────────────────────────────────────────┐  │
   │  │ SessionCleanupWorker (30분 cron)               │  │
   │  └────────────────────────────────────────────────┘  │
   └────────────────┬─────────────────────────────────────┘
                    │ MariaDB native protocol
                    ▼
              ┌──────────────────────┐
              │ MariaDB              │
              │ 10.10.10.100:3306    │
              │ StudySync (8 tables) │
              └──────────────────────┘
```

---

## 2. 기술 스택

| 영역 | 사용 기술 |
|---|---|
| 언어 | C++17 |
| 빌드 | CMake 3.16+ + FetchContent |
| HTTP | [cpp-httplib v0.15.3](https://github.com/yhirose/cpp-httplib) (header-only) |
| JSON | [nlohmann/json v3.11.3](https://github.com/nlohmann/json) |
| JWT | [jwt-cpp v0.7.0](https://github.com/Thalhammer/jwt-cpp) (HS256) |
| 로깅 | 자체 logger.h + [spdlog v1.13.0](https://github.com/gabime/spdlog) async 백엔드 |
| DB | MariaDB Connector/C (`libmariadb-dev`) |
| 비밀번호 | bcrypt via glibc `crypt_r()` (`libcrypt`) |
| TCP | raw POSIX socket |

외부 의존성 (cpp-httplib / jwt-cpp / nlohmann_json / spdlog) 은 CMake `FetchContent` 가 빌드 시 자동 다운로드 — 시스템 설치 불필요.

---

## 3. 빌드 / 실행

### 의존성 (Ubuntu 기준)
```bash
sudo apt install -y cmake g++ pkg-config libmariadb-dev libcrypt-dev
```

### 빌드
```bash
cd main-server
cmake -S . -B app -DCMAKE_BUILD_TYPE=Release
cmake --build app -j$(nproc)
```
산출물: `main-server/app/studysync_main_server` (~2 MB).

### 실행
```bash
./app/studysync_main_server ../config/config.json
# 또는
CONFIG_PATH=/path/config.json ./app/studysync_main_server
JWT_SECRET=real-secret ./app/studysync_main_server ../config/config.json
```

### 부팅 로그 예시 (정상)
```
🔄 [MAIN ] config 로드 완료 | ../config/config.json (21 값, 1 배열)
🔄 [MAIN ] EventBus 시작 | 워커=4개
📊 [DB   ] 커넥션 풀 초기화 완료 | 4개 연결 | 10.10.10.100:3306/StudySync
🔄 [MAIN ] AI수신 리스너 시작 | 포트=9000
🔄 [MAIN ] HTTP 서버 시작 | 0.0.0.0:8081
🔄 [MAIN ] SessionCleanupWorker 시작 | interval=30분 stale=6h
🔄 [MAIN ] StudySync 메인서버 시작 완료 | TCP=9000 HTTP=8081 | Ctrl+C 종료
```

---

## 4. 설정 (`config/config.json`)

| 키 | 기본값 | 설명 |
|---|---|---|
| `network.http_host` | `0.0.0.0` | HTTP listen 주소 |
| `network.http_port` | `8081` | HTTP 포트. 8080은 mongglemonggle 충돌로 8081 정착 |
| `network.main_server_ai_port` | `9000` | AI/학습 서버 측 TCP listen |
| `network.training_server_host` | `10.10.10.120` | 헬스체크 대상 |
| `network.allowed_ip_prefixes` | `["127.","10.","192.168.","172."]` | TCP 화이트리스트 |
| `database.host` | `10.10.10.100` | MariaDB |
| `database.user` | `admin` | |
| `database.password` | `1234` | (개발용 — 운영시 환경변수 분리 권장) |
| `database.schema` | `StudySync` | |
| `database.pool_size` | `4` | 커넥션 풀 크기 |
| `auth.jwt_secret` | `studysync-dev-secret-please-change` | HS256 시크릿. **환경변수 `JWT_SECRET` 우선** |
| `auth.jwt_expires_sec` | `86400` | 24시간 |
| `limits.session_cleanup_interval_min` | `30` | cron 주기 |
| `limits.session_stale_hours` | `6` | 미종료 세션 강제 마감 임계 |
| `health_targets[]` | (배열) | HealthChecker 가 5초마다 ping |

---

## 5. DB 스키마 (8 테이블)

`main-server/sql/schema.sql` 참고. 핵심 컬럼만:

| 테이블 | 주요 컬럼 | 비고 |
|---|---|---|
| `users` | id BIGINT PK, email UNIQUE, password_hash, name, role | role 기본 'user' |
| `goals` | user_id UNIQUE, daily_goal_min, rest_interval_min, rest_duration_min, updated_at | upsert |
| `sessions` | user_id, date, start_time, end_time, focus_min FLOAT, avg_focus FLOAT, goal_achieved | end_time NULL = 진행 중 |
| `focus_logs` | session_id FK, ts, timestamp_ms BIGINT, focus_score INT 0~100, state, is_absent, is_drowsy, ear, neck_angle, shoulder_diff, head_yaw, head_pitch, face_detected, phone_detected | CHECK(focus_score 0~100) |
| `posture_logs` | session_id FK, ts, timestamp_ms, neck_angle FLOAT, shoulder_diff FLOAT, posture_ok, vs_baseline | NULL 허용 컬럼 다수 |
| `posture_events` | event_id UNIQUE, session_id FK, event_type, severity, clip_id, clip_access, clip_ref, retention_days, expires_at_ms | 멱등 INSERT, Claim Check |
| `train_data` | user_id FK, ts, landmarks_json, label, used_for_training | label IN ('focus','distracted','drowsy') |
| `models` | model_type, version UNIQUE per type, accuracy, file_path, is_active | 학습 완료 시 등록 |

`state` 값은 영어 enum: `focus` / `distracted` / `drowsy` (ai_client_tcp_protocol.md §6 합의).

---

## 6. HTTP API (13 endpoints)

베이스 URL: `http://<server-ip>:8081`

모든 응답은 `{ "code": <int>, "message": "...", ... }` 표준. 401/4xx 에러는 `detail` alias 도 같이 박힘 (클라 fallback 호환).

### 6.1 인증

| Method | Path | Body | Response 핵심 | 인증 |
|---|---|---|---|---|
| POST | `/auth/register` | `{email, password, name}` | 201 `{user_id}` / 409 conflict / 400 invalid | ✗ |
| POST | `/auth/login` | `{email, password}` | 200 `{access_token, token, user_id, name}` / 401 | ✗ |

JWT 클레임:
```json
{
  "sub": "42", "email": "...", "name": "...", "role": "user",
  "iat": 1746500000, "exp": 1746586400, "iss": "studysync"
}
```

### 6.2 목표

| Method | Path | Body | Response | 인증 |
|---|---|---|---|---|
| POST | `/goal` | `{daily_goal_min, rest_interval_min, rest_duration_min}` | 200 ok | ✓ |
| GET | `/goal` | — | 200 `{daily_goal_min, rest_interval_min, rest_duration_min, updated_at}` / 404 | ✓ |

### 6.3 세션

| Method | Path | Body | Response | 인증 |
|---|---|---|---|---|
| POST | `/session/start` | `{start_time}` (ISO8601) | 200 `{session_id}` | ✓ |
| POST | `/session/end` | `{session_id, end_time}` | 200 `{focus_min INT, avg_focus FLOAT, goal_achieved BOOL}` | ✓ |

`/session/start` 는 호출 시 같은 user 의 미종료 세션을 자동 마감 (orphan 정리 — §8 참고).

### 6.4 로그 인입

| Method | Path | Body | Response | 인증 |
|---|---|---|---|---|
| POST | `/log/ingest` ★ | NDJSON (application/x-ndjson) | 200 `{accepted: {analysis, event}, skipped}` / 500 ROLLBACK | ✓ |
| POST | `/focus` | `{session_id, logs[]}` 또는 단건 | 200 `{inserted}` | ✓ |
| POST | `/posture` | 동일 패턴 | 200 `{inserted}` | ✓ |

★ 클라 권장 경로. `/focus`/`/posture` 는 단일 도메인 보조 (대부분 미사용).

### 6.5 통계

| Method | Path | Query | Response | 인증 |
|---|---|---|---|---|
| GET | `/stats/today` | — | `{focus_min, avg_focus 0~1, warning_count, goal_progress}` | ✓ |
| GET | `/stats/hourly` | `?date=YYYY-MM-DD` | `{data: [{hour, avg_focus}]}` | ✓ |
| GET | `/stats/pattern` | — | `{avg_focus_duration, best_hour, weekly_avg}` | ✓ |
| GET | `/stats/weekly` | — | `{data: [{date, focus_min, avg_focus}]}` | ✓ |

### 6.6 운영

| Method | Path | Response |
|---|---|---|
| GET | `/health` | 200 `{code:200, message:"ok"}` |
| OPTIONS | `*` | 204 (CORS preflight) |

모든 요청은 access log 에 기록됨:
```
🔄 [MAIN ] HTTP POST /auth/login from 10.10.10.131 (body=45)
```

---

## 7. /log/ingest NDJSON 형식

본문은 줄 단위 JSON. 30개 묶음 권장 (클라 [JsonlBatchUploader](../client/src/network/JsonlBatchUploader.cpp) 기본값).

### Analysis line (5fps 분석 결과)
```json
{"kind":"analysis","session_id":42,"timestamp_ms":1778115600000,
 "focus_score":85,"state":"focus","posture_ok":true,
 "neck_angle":12.3,"shoulder_diff":5.1,
 "drowsy":false,"absent":false}
```
→ `focus_logs` 1행 + `posture_logs` 1행 INSERT.

키 별칭: `drowsy`/`is_drowsy`, `absent`/`is_absent` 둘 다 수용.

### Event line (Claim Check)
```json
{"kind":"event","session_id":42,
 "event_id":"evt-42-1778115660000","event_type":"drowsy",
 "severity":"warning","reason":"drowsy detected",
 "timestamp_ms":1778115660000,
 "clip_id":"local:event_1778115660000",
 "clip_ref":"local:event_1778115660000",
 "clip_access":"local_only","clip_format":"jpeg_sequence",
 "frame_count":90,"retention_days":3,
 "expires_at_ms":1778374860000}
```
→ `posture_events` 1행 INSERT (event_id UNIQUE 멱등).

### 트랜잭션 보장
- 30 line 모두 단일 커넥션 + autocommit OFF 로 묶음
- 어느 하나 실패 시 → ROLLBACK + 500 응답
- 모두 성공 시 → COMMIT + 200 응답
- 멱등 (중복 event_id) 은 성공으로 카운트

응답 예:
```json
{"code":200,"message":"ok",
 "accepted":{"analysis":25,"event":5},"skipped":0}
```

---

## 8. 세션 라이프사이클 + 자동 정리

```
[클라] POST /session/start
       ↓
       SessionService::start_with_cleanup
         ├─ SessionDao::force_close_user_open_sessions  ← 같은 user 의 미종료 세션 자동 마감
         └─ SessionDao::start                            ← 새 row 생성

[클라] POST /log/ingest (반복)
       ↓ /log/ingest 트랜잭션 INSERT

[클라] POST /session/end
       ↓
       SessionService::end
         ├─ SessionDao::aggregate    ← focus_logs 집계
         ├─ GoalDao::find_by_user    ← 목표 비교
         └─ SessionDao::end          ← UPDATE end_time/focus_min/avg_focus/goal_achieved
```

### Auto-cleanup 두 단계

1. **/session/start 시 자동 마감** — 같은 user 의 `end_time IS NULL` 세션을 모두 강제 마감 후 새 세션 생성. 클라 비정상 종료 후 재시작 시나리오 안전 처리.

2. **SessionCleanupWorker (30분 cron)** — 별도 스레드가 주기적으로 모든 사용자에 대해 `start_time < NOW() - INTERVAL 6 HOUR AND end_time IS NULL` 세션 강제 마감. 예: PC 끄고 그대로 둔 세션 6시간 후 정리.

두 정리 모두 마감 시 `SessionDao::aggregate` 로 진짜 데이터 집계해 `focus_min`/`avg_focus` 채움.

---

## 9. TCP 프로토콜 (포트 9000)

### 활성 채널
| Protocol | 방향 | 용도 |
|---|---|---|
| `TRAIN_PROGRESS` (1102) | 학습→메인 | 진행률 |
| `TRAIN_COMPLETE` (1104) | 학습→메인 | 모델 바이너리 + 메타 |
| `TRAIN_COMPLETE_ACK` (1105) | 메인→학습 | ACK |
| `TRAIN_FAIL` (1106) / `TRAIN_FAIL_ACK` (1107) | 양방향 | |
| `MODEL_RELOAD_CMD` (1010) | 메인→AI | 새 모델 동봉 |
| `HEALTH_PING` (1200) / `HEALTH_PONG` (1201) | 양방향 | 5초 주기 |

### 도메인 채널 (선택적 — 클라가 보통 `/log/ingest` HTTP 사용)
| Protocol | 방향 | 용도 |
|---|---|---|
| `FOCUS_LOG_PUSH` (1000) / `_ACK` (1001) | AI→메인 | 집중도 푸시 |
| `POSTURE_LOG_PUSH` (1002) / `_ACK` (1003) | AI→메인 | 자세 푸시 |
| `POSTURE_EVENT_PUSH` (1004) / `_ACK` (1005) | AI→메인 | 이벤트 |
| `BASELINE_CAPTURE_PUSH` (1006) | AI→메인 | (미사용) |

도메인 채널은 `FocusService` / `PostureService` 가 EventBus 구독으로 처리. 클라가 HTTP 만 쓰면 dead path 지만 코드 유지.

### 패킷 포맷
```
[4바이트 BE JSON 길이][JSON 본문][바이너리(옵션, image_size 바이트)]
```

JSON 공통 필드: `protocol_no`, `protocol_version="1.0"`, `request_id`, `timestamp`, `image_size`.

응답 ACK JSON 키:
```json
{"protocol_no": 1001, "protocol_version": "1.0",
 "request_id": "...", "ack": true,
 "image_size": 0}
```

---

## 10. 인증 / 보안

- **JWT HS256** — secret 환경변수 `JWT_SECRET` 우선, 미설정 시 config 값
- **Bearer 헤더** — 모든 보호 경로에서 `Authorization: Bearer <token>` 검증
- **bcrypt** — 비밀번호 저장 (`PasswordHash::hash` / `verify`, glibc crypt_r)
- **Prepared statements** — 모든 DAO 가 `mysql_stmt_prepare` 사용 (SQL Injection 차단)
- **세션 소유권 검증** — `/log/ingest` 등에서 `session_id` 가 JWT 의 user 소유인지 확인
- **CORS** — 개발 편의로 `*` 전체 허용 (운영 시 도메인 화이트리스트 권장)
- **TCP IP 화이트리스트** — `network.allowed_ip_prefixes` 로 9000 포트 접근 제한

운영 미반영:
- TLS / HTTPS (nginx reverse proxy 권장)
- rate limiting
- 비밀번호 강도 정책 (현재 4자 이상)
- DB 비밀번호 암호화

---

## 11. 로그 시스템

```
출력 → ① stdout (즉시)
       ② logs/YYYY-MM-DD.log (자체 일자별 회전)
       ③ spdlog 비동기 백엔드:
          - logs/error.log (warn 이상)
          - logs/main-rotating.log (50MB × 5 회전)
```

태그/이모지 (`logger.h`):
- `🔄 [MAIN]` 일반
- `📊 [DB  ]` DB 작업
- `🧠 [AI  ]` AI 인입
- `👤 [CLT ]` 클라 인증
- `🟩 [ACK ]` ACK 송신
- `🚀 [TRAIN]` 학습
- `❌` 에러 (자동으로 stderr + error.log)

호출부:
```cpp
log_main("HTTP POST %s from %s", path, addr);
log_db("INSERT users | id=%lld", id);
log_err_db("prepare 실패 | %s", mysql_stmt_error(stmt));
```

---

## 12. 디렉토리 구조

```
main-server/
├─ CMakeLists.txt              ← FetchContent (cpp-httplib/jwt-cpp/spdlog/nlohmann)
├─ app/                        ← 빌드 산출물 (gitignore)
│   └─ studysync_main_server   ← 실행 파일
├─ common/
│   └─ Protocol.h              ← TCP enum + 패킷 포맷
├─ include/
│   ├─ core/
│   │   ├─ config.h            ← config.json 로더
│   │   ├─ event_bus.h         ← Pub/Sub 허브
│   │   ├─ event_types.h       ← TCP 이벤트 페이로드 정의
│   │   ├─ logger.h            ← 자체 로거 + spdlog 백엔드
│   │   ├─ tcp_listener.h      ← :9000 인입
│   │   └─ tcp_utils.h
│   ├─ handler/
│   │   ├─ ack_sender.h        ← TCP ACK 송신
│   │   ├─ router.h            ← protocol_no 분기
│   │   └─ train_handler.h
│   ├─ http/
│   │   ├─ http_server.h       ← cpp-httplib 래퍼 + access log
│   │   ├─ jwt_middleware.h    ← Bearer 검증/발급
│   │   ├─ error_response.h    ← 표준 응답 헬퍼
│   │   └─ controllers/
│   │       ├─ auth_controller.h
│   │       ├─ goal_controller.h
│   │       ├─ session_controller.h
│   │       ├─ log_controller.h        ← /focus, /posture
│   │       ├─ log_ingest_controller.h ← /log/ingest NDJSON
│   │       └─ stats_controller.h
│   ├─ monitor/
│   │   ├─ connection_registry.h
│   │   └─ health_checker.h
│   ├─ security/
│   │   ├─ input_validator.h
│   │   ├─ ip_filter.h
│   │   └─ json_safety.h
│   ├─ service/
│   │   ├─ user_service.h
│   │   ├─ goal_service.h
│   │   ├─ session_service.h           ← start_with_cleanup
│   │   ├─ session_cleanup_worker.h    ← 30분 cron
│   │   ├─ log_service.h               ← ingest_transactional
│   │   ├─ stats_service.h
│   │   ├─ focus_service.h             ← TCP 1000 dead path
│   │   ├─ posture_service.h           ← TCP 1002/1004 dead path
│   │   └─ train_service.h
│   └─ storage/
│       ├─ connection_pool.h           ← MariaDB 풀
│       ├─ dao.h                       ← 8 DAO 인터페이스
│       └─ password_hash.h             ← bcrypt
├─ src/                                 ← 위 헤더 .cpp 구현
└─ sql/
    └─ schema.sql                       ← StudySync DDL
```

---

## 13. 데이터 흐름 시나리오

### 정상 학습 세션 (클라 → 메인)
```
1. POST /auth/login        → JWT 받음
2. POST /goal              → 사용자 목표 저장
3. POST /session/start     → session_id=N 받음 (이전 미종료 자동 정리)
4. (5fps 분석 결과 누적) → 30개씩 NDJSON
5. POST /log/ingest        → 25 analysis + 5 event 트랜잭션 INSERT
6. ... 4-5 반복 ...
7. POST /session/end       → focus_min/avg_focus 집계 + UPDATE
8. GET /stats/today        → 오늘 결과 조회
```

### 비정상 종료 (클라 크래시 후)
```
- 다음 로그인 시 POST /session/start
  → 이전 미종료 세션 자동 마감 (focus_logs 집계 → end_time=now)
  → 새 세션 생성
- 6시간 그대로 PC 끄면 SessionCleanupWorker 가 자동 정리
```

### 학습 모델 배포 (학습 서버 → 메인 → AI)
```
1. 학습 서버 → TRAIN_COMPLETE (1104) + 모델 바이너리
2. TrainService:
   - validate (request_id, version, accuracy, size)
   - 모델 파일 atomic 저장 (.tmp.{pid}.{ns} → rename)
   - models 테이블 INSERT (이전 같은 model_type 활성 모델 비활성화)
3. 메인 → 학습 ACK (1105)
4. 메인 → AI MODEL_RELOAD_CMD (1010) + 모델 바이너리
```

---

## 14. 알려진 제약 / 미합의 (TODO)

코드에 `TODO(spec)` 주석 박혀있음. 합의 후 SQL/로직 정정 필요.

| # | 항목 | 잠정값 | 위치 |
|---|---|---|---|
| 1 | `focus_min` 정확한 정의 | `state='focus'` 행 수 × 0.2초 / 60 | dao.cpp aggregate |
| 2 | `goal_achieved` 정의 | 단일 세션 ≥ daily_goal_min | session_service.cpp |
| 3 | `warning_count` 의미 | `posture_logs(posture_ok=0)` 카운트 | dao.cpp get_today |
| 4 | `avg_focus_duration / best_hour / weekly_avg` 의미 | 잠정 SQL | dao.cpp get_pattern |
| 5 | `focus_score` 0~1 vs 0~100 | INT 0~100 정착 (DB 기준) | dao.h, log_controller |

운영 영역 (안 한다고 결정됨):
- TLS / HTTPS
- systemd / Docker
- JWT_SECRET 운영 값
- rate limiting
- 비밀번호 강도 정책
- TRAIN_DATA HTTP 엔드포인트 (DAO 만 있음)
- request_id thread_local 추적
- integration test 자동화

---

## 15. 트러블슈팅

### 8081 또는 9000 포트 바인드 실패
```bash
ss -tlnp | grep 8081      # 누가 점유 중인지
pkill -f studysync_main_server  # 이전 인스턴스 종료
```

### 8080 충돌 (mongglemonggle 등 다른 서비스)
config.json 의 `network.http_port` 변경 → 클라 `main_server_url` 도 동일 포트로 갱신.

### DB 도달 실패
```bash
ping 10.10.10.100
mysql -h10.10.10.100 -u admin -p1234 -e "SHOW DATABASES;"
```

### 클라가 404 받음
- 클라 `main_server_url` 이 메인서버 IP:포트 맞는지 확인
- 메인서버 access log 에 요청이 도달하는지 확인 — `🔄 HTTP POST /<path>` 줄
- path 에 `http://...` 통째로 박힌 형태면 클라 측 URL 파싱 버그

### 미종료 세션 누적
정상이면 SessionCleanupWorker 가 30분마다 자동 정리. 직접 정리:
```sql
UPDATE sessions SET end_time=NOW() WHERE end_time IS NULL AND start_time < NOW() - INTERVAL 6 HOUR;
```

### 토큰 만료 (24시간)
클라가 401 받으면 재로그인. JWT 만료 시간은 `auth.jwt_expires_sec` 로 조정.

---

## 16. 빠른 검증 (curl)

```bash
# 헬스체크
curl http://10.10.10.130:8081/health

# 회원가입 + 로그인
curl -X POST http://10.10.10.130:8081/auth/register \
  -H "Content-Type: application/json" \
  -d '{"email":"a@b.c","password":"pw1234","name":"테스트"}'

TOKEN=$(curl -sX POST http://10.10.10.130:8081/auth/login \
  -H "Content-Type: application/json" \
  -d '{"email":"a@b.c","password":"pw1234"}' | jq -r .access_token)

# 세션 시작
SID=$(curl -sX POST http://10.10.10.130:8081/session/start \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"start_time":"2026-05-07T10:00:00+09:00"}' | jq -r .session_id)

# /log/ingest 시뮬레이션
echo "{\"kind\":\"analysis\",\"session_id\":$SID,\"timestamp_ms\":1778115600000,\"focus_score\":85,\"state\":\"focus\",\"posture_ok\":true,\"neck_angle\":12.3,\"shoulder_diff\":5.1,\"drowsy\":false,\"absent\":false}" | \
  curl -X POST http://10.10.10.130:8081/log/ingest \
    -H "Authorization: Bearer $TOKEN" \
    -H "Content-Type: application/x-ndjson" \
    --data-binary @-

# 세션 종료 + 통계
curl -X POST http://10.10.10.130:8081/session/end \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d "{\"session_id\":$SID,\"end_time\":\"2026-05-07T11:00:00+09:00\"}"

curl http://10.10.10.130:8081/stats/today \
  -H "Authorization: Bearer $TOKEN"
```

---

## 17. 변경 이력 요약

| 일자 | 내용 |
|---|---|
| 2026-05-06 | 공장 QC C++ 골격 → StudySync 도메인 전환 (8 DAO 신설, schema 교체) |
| 2026-05-06 | HTTP 레이어 13 endpoints + JWT + spdlog 백엔드 |
| 2026-05-07 | `/log/ingest` NDJSON + 클라 호환 응답 정렬 (access_token, focus_min INT) |
| 2026-05-07 | feature/taehyun 머지 (클라 NDJSON POST 구현 + HUD + DummyAnalysis) |
| 2026-05-07 | HTTP port 8080→8081 (mongglemonggle 충돌 회피) + access log 추가 |
| 2026-05-07 | `/log/ingest` 트랜잭션 + `/session/start` 자동 정리 + 6h cron worker |

---

## 18. 관련 문서

- 클라 측 API 명세: [client_api_jwt_spec.md](../docs/client_api_jwt_spec.md)
- 클라 ↔ AI TCP 프로토콜: [ai_client_tcp_protocol.md](../docs/ai_client_tcp_protocol.md)
- 클라 측 진행 현황: [docs/client/](../docs/client/)
- DB ERD 원본: [docs/project/](../docs/project/)
