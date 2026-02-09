from flask import Flask, request
import os
import datetime
import subprocess

app = Flask(__name__)
UPLOAD_FOLDER = "uploads"
os.makedirs(UPLOAD_FOLDER, exist_ok=True)


@app.route("/upload", methods=["POST"])
def upload_file():
    print(f"Received request from {request.remote_addr}")

    timestamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    filename = f"rec_{timestamp}.wav"
    filepath = os.path.join(UPLOAD_FOLDER, filename)

    # Save the raw audio data
    data = request.get_data()
    with open(filepath, "wb") as f:
        f.write(data)

    print(f" Saved: {filepath} ({len(data)} bytes)")

    # Play the audio (Linux ALSA) - Optional, for debugging
    try:
        subprocess.run(["aplay", filepath], check=False)
    except Exception as e:
        print(f"Could not play audio: {e}")

    return "Upload Successful", 200


if __name__ == "__main__":
    print("Starting server on 0.0.0.0:5000...")
    app.run(host="0.0.0.0", port=5000, debug=True)
