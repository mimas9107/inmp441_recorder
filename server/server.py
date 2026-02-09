from flask import Flask, request
import os
import datetime
import subprocess
import threading

app = Flask(__name__)
UPLOAD_FOLDER = "uploads"
os.makedirs(UPLOAD_FOLDER, exist_ok=True)

# Counter for feature01a dataset collection
sample_count = 1
count_lock = threading.Lock()


@app.route("/upload", methods=["POST"])
def upload_file():
    global sample_count
    print(f"Received request from {request.remote_addr}")

    with count_lock:
        filename = f"sample{sample_count}.wav"
        sample_count += 1

    filepath = os.path.join(UPLOAD_FOLDER, filename)

    # Save the raw audio data
    data = request.get_data()
    with open(filepath, "wb") as f:
        f.write(data)

    print(f" Saved: {filepath} ({len(data)} bytes)")

    # Play the audio (Optional)
    try:
        subprocess.run(["aplay", filepath], check=False)
    except Exception as e:
        pass

    return f"Saved as {filename}", 200


if __name__ == "__main__":
    print("Starting server on 0.0.0.0:5000...")
    app.run(host="0.0.0.0", port=5000, debug=True)
