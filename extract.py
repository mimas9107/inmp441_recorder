import base64, re

with open("record.log","r") as f:
    t=f.read()

m=re.search(r"===WAV_START===.*?SIZE:\d+(.*?)===WAV_END===",t, re.S)
b64=m.group(1).replace("\n","").strip()

open("record.wav","wb").write(base64.b64decode(b64))
print("saved record.wav")

