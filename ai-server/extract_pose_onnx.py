"""
mediapipe 0.10.14의 pose_landmark_full.tflite를 ONNX로 변환해
클라이언트 models/ 폴더에 복사하는 스크립트.

실행: python extract_pose_onnx.py
"""

import os
import subprocess
import shutil
import sys

MEDIAPIPE_ROOT = os.path.dirname(__import__("mediapipe").__file__)
TFLITE_SRC = os.path.join(MEDIAPIPE_ROOT, "modules/pose_landmark/pose_landmark_full.tflite")

# 스크립트 위치 기준으로 client/models 경로 계산
SCRIPT_DIR   = os.path.dirname(os.path.abspath(__file__))
CLIENT_MODELS = os.path.join(SCRIPT_DIR, "..", "client", "models")
ONNX_DST     = os.path.join(CLIENT_MODELS, "pose_landmark.onnx")
ONNX_TMP     = "/tmp/pose_landmark_new.onnx"

def run(cmd):
    print(">>", " ".join(cmd))
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print("STDERR:", result.stderr[-2000:])
        raise RuntimeError(f"Command failed: {' '.join(cmd)}")
    return result.stdout

def main():
    if not os.path.exists(TFLITE_SRC):
        print(f"[ERROR] TFLite 파일을 찾을 수 없음: {TFLITE_SRC}")
        sys.exit(1)
    print(f"[OK] TFLite 파일 발견: {TFLITE_SRC}")

    # tf2onnx로 변환
    run([
        sys.executable, "-m", "tf2onnx.convert",
        "--tflite", TFLITE_SRC,
        "--output", ONNX_TMP,
        "--opset", "13",
    ])
    print(f"[OK] ONNX 변환 완료: {ONNX_TMP}")

    # 기존 파일 백업
    if os.path.exists(ONNX_DST):
        backup = ONNX_DST + ".bak"
        shutil.copy2(ONNX_DST, backup)
        print(f"[OK] 기존 파일 백업: {backup}")

    # 복사
    os.makedirs(CLIENT_MODELS, exist_ok=True)
    shutil.copy2(ONNX_TMP, ONNX_DST)
    print(f"[OK] 복사 완료: {ONNX_DST}")

    # 출력 노드 이름 확인 (클라이언트 로그와 비교용)
    try:
        import onnx
        model = onnx.load(ONNX_DST)
        outputs = [o.name for o in model.graph.output]
        print(f"[INFO] ONNX 출력 노드: {outputs}")
        print("       클라이언트 DebugView에서 [LocalPose] pose output 로그와 일치하는지 확인하세요.")
    except ImportError:
        print("[INFO] onnx 패키지 없음 — 노드 이름 확인 생략")

    print("\n완료. 클라이언트를 빌드 후 테스트하세요.")

if __name__ == "__main__":
    main()
