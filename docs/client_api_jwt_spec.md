# StudySync 클라이언트 ↔ 메인서버 API / JWT 명세

> 최초 작성: 2026-05-06  
> 최종 수정: 2026-05-08  
> 작성자: 정태현 (클라이언트)  
> 목적: 메인서버 HTTP 서버 구현 시 클라이언트와 맞춰야 할 스펙 정의

---

## 변경 이력

| 날짜 | 변경 내용 |
|------|-----------|
| 2026-05-06 | 최초 작성 |
| 2026-05-08 | 베이스 URL 수정, `POST /auth/logout` 추가, `POST /log/ingest` NDJSON 추가, AI 서버 HTTP 섹션 → TCP keypoint 프로토콜로 교체, 피드백 API 응답 명확화 |

---

## 1. 공통 사항

### 베이스 URL
```
http://10.10.10.130:8081
```

### 인증 방식
- 로그인·회원가입 제외한 모든 요청에 JWT 필수
- 헤더: `Authorization: Bearer <token>`
- 알고리즘: **HS256**

### 공통 응답 포맷
```json
{ "code": <HTTP 상태코드>, "message": "설명" }
```
성공 시 추가 필드 포함, 실패 시 `message`에 오류 설명.

### 시간 포맷
ISO 8601: `"2026-05-08T14:30:00+09:00"`

---

## 2. JWT 페이로드 (서버 발급 시 포함 클레임)

```json
{
  "sub":   "42",
  "email": "user@example.com",
  "name":  "홍길동",
  "role":  "user",
  "iat":   1746500000,
  "exp":   1746586400
}
```

| 필드 | 타입 | 설명 |
|------|------|------|
| `sub` | string | users.id (DB PK, 문자열) |
| `email` | string | 로그인 이메일 |
| `name` | string | 표시 이름 |
| `role` | string | `"user"` 고정 |
| `iat` | int | 발급 시각 (Unix timestamp) |
| `exp` | int | 만료 시각 (권장: iat + 86400, 24시간) |

> 클라이언트는 토큰을 `%APPDATA%\StudySync\token.dat`에 저장한다.  
> 만료된 토큰으로 요청 시 서버는 401을 반환하고, 클라이언트는 재로그인을 유도한다.

---

## 3. API 엔드포인트 명세

### 3-1. 회원가입

```
POST /auth/register
Content-Type: application/json
```

**Request Body**
```json
{
  "email":    "user@example.com",
  "password": "plaintext_password",
  "name":     "홍길동"
}
```

**Response**
```json
// 201 Created (성공)
{ "code": 201, "message": "ok", "user_id": 42 }

// 409 Conflict (이메일 중복)
{ "code": 409, "message": "email already exists" }

// 400 Bad Request (필드 누락/형식 오류)
{ "code": 400, "message": "invalid email format" }
```

---

### 3-2. 로그인

```
POST /auth/login
Content-Type: application/json
```

**Request Body**
```json
{
  "email":    "user@example.com",
  "password": "plaintext_password"
}
```

**Response**
```json
// 200 OK (성공)
{
  "code":    200,
  "token":   "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...",
  "user_id": 42,
  "name":    "홍길동"
}

// 401 Unauthorized (이메일/비밀번호 불일치)
{ "code": 401, "message": "invalid credentials" }
```

---

### 3-3. 로그아웃 ⭐ 신규

```
POST /auth/logout
Authorization: Bearer <token>
```

**Request Body** — 없음 (빈 body)

**Response**
```json
// 200 OK
{ "code": 200, "message": "logged out" }

// 401 Unauthorized (토큰 없음/만료)
{ "code": 401, "message": "unauthorized" }
```

> **서버 구현 요청사항**  
> 클라이언트는 설정 탭 로그아웃 버튼 클릭 시 이 엔드포인트를 호출한 뒤  
> 로컬 토큰(`token.dat`)을 삭제하고 앱을 종료한다.  
> 서버는 해당 토큰을 블랙리스트 처리하거나 DB에서 무효화 처리해야 한다.  
> (stateless JWT 방식이라면 만료 전 무효화를 위해 블랙리스트 테이블 필요)

---

### 3-4. 학습 목표 설정

```
POST /goal
Authorization: Bearer <token>
Content-Type: application/json
```

**Request Body**
```json
{
  "daily_goal_min":    120,
  "rest_interval_min": 50,
  "rest_duration_min": 10
}
```

**Response**
```json
// 200 OK
{ "code": 200, "message": "ok" }
```

---

### 3-5. 학습 목표 조회

```
GET /goal
Authorization: Bearer <token>
```

**Response**
```json
// 200 OK
{
  "code": 200,
  "daily_goal_min":    120,
  "rest_interval_min": 50,
  "rest_duration_min": 10,
  "updated_at": "2026-05-08T10:00:00+09:00"
}

// 404 (목표 미설정)
{ "code": 404, "message": "goal not set" }
```

---

### 3-6. 세션 시작

```
POST /session/start
Authorization: Bearer <token>
Content-Type: application/json
```

**Request Body**
```json
{
  "start_time": "2026-05-08T14:30:00+09:00"
}
```

**Response**
```json
// 200 OK
{ "code": 200, "session_id": 1001 }
```

> 클라이언트는 반환된 `session_id`를  
> ① AI 서버 TCP keypoint 패킷, ② `/log/ingest` NDJSON 라인에 포함해 전송한다.  
> 세션별 클립 디렉토리: `event_clips/{session_id}/`

---

### 3-7. 세션 종료

```
POST /session/end
Authorization: Bearer <token>
Content-Type: application/json
```

**Request Body**
```json
{
  "session_id": 1001,
  "end_time":   "2026-05-08T16:00:00+09:00"
}
```

**Response**
```json
// 200 OK (서버가 logs 집계 후 반환)
{
  "code":          200,
  "focus_min":     72,
  "avg_focus":     0.83,
  "goal_achieved": true
}
```

| 필드 | 타입 | 설명 |
|------|------|------|
| `focus_min` | int | 해당 세션의 집중 시간 (분) |
| `avg_focus` | float | 평균 집중도 (0.0 ~ 1.0) |
| `goal_achieved` | bool | 일일 목표 달성 여부 |

---

### 3-8. 분석·이벤트 로그 일괄 업로드 ⭐ 신규

```
POST /log/ingest
Authorization: Bearer <token>
Content-Type: application/x-ndjson
```

클라이언트는 분석 결과와 이상 이벤트를 **NDJSON**(Newline-Delimited JSON) 형식으로  
10초마다 일괄 전송한다. 한 요청에 분석 라인과 이벤트 라인이 혼재할 수 있다.

#### 분석 결과 라인 (`"kind": "analysis"`)

```json
{
  "kind":         "analysis",
  "session_id":   1001,
  "timestamp_ms": 1746514205123,
  "focus_score":  85,
  "state":        "focus",
  "ear":          0.32,
  "neck_angle":   12.5,
  "shoulder_diff": 5.1,
  "head_yaw":     -3.2,
  "head_pitch":    8.1,
  "face_detected": 1,
  "posture_ok":   true,
  "drowsy":       false,
  "absent":       false,
  "confidence":   0.92
}
```

#### 이벤트 라인 — 클립 없음 (`"kind": "event"`)

```json
{
  "kind":         "event",
  "session_id":   1001,
  "event_id":     "evt-1746514205123",
  "event_type":   "drowsy",
  "timestamp_ms": 1746514205123,
  "reason":       "eye closure detected",
  "confidence":   0.87,
  "frame_count":  15
}
```

#### 이벤트 라인 — 로컬 클립 포함 (`"kind": "event"`, `"clip_access": "local_only"`)

```json
{
  "kind":           "event",
  "session_id":     1001,
  "event_id":       "evt-1746514205123",
  "event_type":     "drowsy",
  "timestamp_ms":   1746514205123,
  "reason":         "eye closure detected",
  "frame_count":    15,
  "clip_id":        "evt-1746514205123",
  "clip_ref":       "event_clips/1001/evt-1746514205123/clip.mp4",
  "clip_access":    "local_only",
  "clip_format":    "mp4",
  "retention_days": 3,
  "created_at_ms":  1746514205123,
  "expires_at_ms":  1746773405123
}
```

##### `event_type` 값 목록

| 값 | 의미 |
|----|------|
| `"drowsy"` | 졸음 감지 |
| `"absent"` | 자리 비움 감지 |
| `"distracted"` | 다른 행동 감지 (자세 이상 포함) |

##### `state` 값 목록 (analysis 라인)

| 값 | 의미 |
|----|------|
| `"focus"` | 집중 중 |
| `"drowsy"` | 졸음 |
| `"absent"` | 자리 비움 |
| `"distracted"` | 다른 행동 / 자세 불량 |

**Response**
```json
// 200 OK
{
  "accepted": {
    "analysis": 25,
    "event":     5
  },
  "skipped": 0
}
```

| 필드 | 타입 | 설명 |
|------|------|------|
| `accepted.analysis` | int | 저장된 분석 라인 수 |
| `accepted.event` | int | 저장된 이벤트 라인 수 |
| `skipped` | int | 중복·유효성 오류로 무시된 라인 수 |

---

### 3-9. 이벤트 피드백 제출

```
POST /feedback
Authorization: Bearer <token>
Content-Type: multipart/form-data
```

사용자가 복기 화면에서 AI 판정이 틀렸다고 응답할 때 전송한다.

**Form Fields**

| 필드 | 타입 | 필수 | 설명 |
|------|------|------|------|
| `event_id` | text | ✅ | 이벤트 식별자 (예: `"evt-1746514205123"`) |
| `session_id` | text | ✅ | 세션 ID |
| `model_pred` | text | ✅ | AI 예측값 (예: `"drowsy"`, `"bad_posture"`) |
| `user_feedback` | text | ✅ | `"correct"` 또는 `"wrong"` |
| `consent_ver` | text | ✅ | 동의 버전 (예: `"v1.0"`) |
| `clip` | file | ❌ | MP4 클립 파일 (있을 때만 포함) |

**Response**
```json
// 200 OK
{ "saved": true }

// 400 Bad Request (필드 누락)
{ "saved": false, "message": "missing event_id" }
```

---

### 3-10. 오늘 통계

```
GET /stats/today
Authorization: Bearer <token>
```

**Response**
```json
// 200 OK
{
  "code":          200,
  "focus_min":     90,
  "avg_focus":     0.78,
  "warning_count": 3,
  "goal_progress": 0.75
}
```

| 필드 | 타입 | 설명 |
|------|------|------|
| `focus_min` | int | 오늘 총 집중 시간 (분) |
| `avg_focus` | float | 오늘 평균 집중도 (0.0~1.0) |
| `warning_count` | int | 오늘 자세·졸음 경고 횟수 |
| `goal_progress` | float | 일일 목표 달성률 (0.0~1.0) |

> **필드명 주의**: 클라이언트는 `focus_min` / `today_focus_min` / `focus_minutes` / `study_min` 중  
> 첫 번째로 발견되는 값을 사용한다. 서버는 반드시 `focus_min`으로 통일할 것.

---

### 3-11. 시간대별 집중도

```
GET /stats/hourly?date=2026-05-08
Authorization: Bearer <token>
```

**Response**
```json
// 200 OK
{
  "code": 200,
  "data": [
    { "hour": 9,  "avg_focus": 0.82 },
    { "hour": 10, "avg_focus": 0.75 },
    { "hour": 14, "avg_focus": 0.90 }
  ]
}
```

---

### 3-12. 집중 패턴 분석

```
GET /stats/pattern
Authorization: Bearer <token>
```

**Response**
```json
// 200 OK
{
  "code":                 200,
  "avg_focus_duration":   47,
  "best_hour":            10,
  "weekly_avg":           0.76
}
```

---

### 3-13. 주간 통계

```
GET /stats/weekly
Authorization: Bearer <token>
```

**Response**
```json
// 200 OK
{
  "code": 200,
  "data": [
    { "date": "2026-04-30", "focus_min": 85,  "avg_focus": 0.80 },
    { "date": "2026-05-01", "focus_min": 120, "avg_focus": 0.75 },
    { "date": "2026-05-08", "focus_min": 90,  "avg_focus": 0.78 }
  ]
}
```

---

## 4. 오류 코드 표

| code | 의미 | 사용 예 |
|------|------|---------|
| 200 | 성공 | 일반 성공 |
| 201 | 생성됨 | 회원가입 |
| 400 | 잘못된 요청 | 필드 누락, 형식 오류 |
| 401 | 인증 실패 | 잘못된 비밀번호, 만료된 JWT |
| 403 | 권한 없음 | 다른 유저 리소스 접근 |
| 404 | 없음 | 목표 미설정, 세션 없음 |
| 409 | 충돌 | 이메일 중복 |
| 500 | 서버 오류 | DB 오류 등 |

---

## 5. AI 서버 TCP keypoint 프로토콜 (클라이언트 ↔ AI 서버)

### 연결 정보
- AI 서버 주소: `10.10.10.50:9100`
- 전송 속도: 약 5fps (30fps 캡처 중 6프레임마다 1회)
- 클라이언트가 로컬에서 MediaPipe ONNX 모델로 keypoint를 추출한 뒤 JSON으로 전송

### 패킷 포맷
```
[4바이트 big-endian JSON 길이] [UTF-8 JSON 본문]
```

### Protocol 2000 — KEYPOINT_PUSH (클라이언트 → AI 서버)

```json
{
  "protocol_no":  2000,
  "session_id":   1001,
  "frame_id":     42,
  "timestamp_ms": 1746514205123,
  "ear":          0.32,
  "neck_angle":   12.5,
  "shoulder_diff": 5.1,
  "head_yaw":     -3.2,
  "head_pitch":    8.1,
  "face_detected": 1
}
```

| 필드 | 타입 | 설명 |
|------|------|------|
| `ear` | float | Eye Aspect Ratio (0=눈 감음, 1=완전히 뜸) |
| `neck_angle` | float | 귀~어깨 수직 편차 각도 (도) |
| `shoulder_diff` | float | 좌우 어깨 y좌표 차이 (px) |
| `head_yaw` | float | 좌우 회전 (-90~+90도) |
| `head_pitch` | float | 앞뒤 기울기 (-90~+90도) |
| `face_detected` | int | 0 또는 1 |

### Protocol 2001 — ANALYSIS_RES (AI 서버 → 클라이언트)

```json
{
  "protocol_no":  2001,
  "session_id":   1001,
  "frame_id":     42,
  "timestamp_ms": 1746514205123,
  "focus_score":  85,
  "state":        "focus",
  "confidence":   0.92,
  "posture_ok":   true,
  "drowsy":       false,
  "absent":       false,
  "guide":        ""
}
```

| 필드 | 타입 | 설명 |
|------|------|------|
| `focus_score` | int | 집중도 점수 (0~100) |
| `state` | string | `"focus"` / `"drowsy"` / `"absent"` / `"distracted"` |
| `confidence` | float | TCN 모델 판정 신뢰도 (0.0~1.0) |
| `guide` | string | 자세 교정 안내 메시지 (정상 시 빈 문자열) |

> AI 서버가 반환하는 판정 결과는 클라이언트에서 추출한 keypoint 위에 덮어씌워져  
> `/log/ingest` NDJSON 라인으로 메인 서버에 전달된다.

---

## 6. 클라이언트 로컬 저장 파일

| 파일 | 위치 | 내용 |
|------|------|------|
| `token.dat` | `%APPDATA%\StudySync\token.dat` | JWT 토큰 (1줄) |
| `consent.dat` | `%APPDATA%\StudySync\consent.dat` | 동의한 버전 문자열 (예: `v1.0`) |
| 클립 영상 | `{exe}\event_clips\{session_id}\{event_id}\clip.mp4` | 이벤트 구간 MP4 클립 |
| 클립 매니페스트 | `{exe}\event_clips\{session_id}\{event_id}\manifest.jsonl` | 클립 메타데이터 |

클립은 `retention_days`(기본 3일) 경과 후 로컬 GC에 의해 자동 삭제된다.

---

## 7. 서버 구현 우선순위 요청

| 우선순위 | 엔드포인트 | 이유 |
|---------|-----------|------|
| 🔴 높음 | `POST /auth/logout` | 현재 토큰 무효화 수단 없음 |
| 🔴 높음 | `POST /log/ingest` (NDJSON) | 분석·이벤트 데이터 실시간 적재 핵심 |
| 🟡 중간 | `GET /stats/today` | HUD 통계 표시용 (클라이언트 60초 주기 polling) |
| 🟡 중간 | `GET /stats/hourly`, `GET /stats/weekly` | 학습 상태 탭 그래프용 |
| 🟢 낮음 | `POST /feedback` | 복기 화면 AI 오답 제보 |
| 🟢 낮음 | `POST/GET /goal` | 목표 설정/조회 (설정 탭 구현 시) |

---

*이 문서는 클라이언트(정태현) 기준으로 작성. 서버 구현 시 이 스펙에 맞춰 응답 형식 통일 요청.*
