sed -n '/===WAV_START===/,/===WAV_END===/p' record.log \
 | sed '1d;$d' \
 | sed '/SIZE:/d' \
 | tr -d '\r\n ' \
 > audio.b64

