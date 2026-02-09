idf.py fullclean build flash
sleep 1
idf.py monitor | tee >(sed -n '/===WAV_START===/,/===WAV_END===/p' \
 | sed '1d;$d' | sed '/SIZE:/d' | tr -d '\r\n ' | base64 -d > record.wav)

