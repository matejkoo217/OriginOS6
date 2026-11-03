!!!DO NOT USE GPT_BOTH0.BIN FROM DIFFRENT DEVICES!!!
THIS SCRIPT IS FOR XIAOMI 13, YOUR gpt_both0.bin MAY DIFFER!
IM NOT RESPONSIBLE FOR BRICKS OR DESTROYED DEVICES!

To easily and safely resize super partition i recommend patching gpt_both0.bin LUA enteries
for super end and userdata start, userdata automatically fills in rest thats why size 
is set to 0 in stock.
Then flash it using:
fastboot -w
fastboot flash partition:0 gpt_both0.bin
fastboot -w
Yes, erasing 2x is safer becouse yes.
🐥

