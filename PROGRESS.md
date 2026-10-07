
# Date : 7 Oct 2026
# Time : 3:16
- Starting the project, copied server.cpp

# Time : 4:00
- made the cpp enviroment.
- implement the custom linked stack.

# Time: 4:33
- Pass 0x0: Validation done
- function declearation strucutre validation done.

# Time: 5:16
- was Learning the binary record format used in resolve.bin: an 8-byte offset, a 4-byte text length, and the source text.
- Understood the difference between a record’s starting position and its stored call-destination offset.

# Break Time

# Time: 8:36
- Learned how ftell() and fwrite() work.
- wrote writeResolveRecord() to save the offset, text length, and text bytes.

# Time: 9:21
- wrote readResolveRecord() with checks for invalid lengths and incomplete reads.
- last func to implment is left. then this phase is done.

