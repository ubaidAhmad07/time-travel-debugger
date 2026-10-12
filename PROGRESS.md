
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

# Time: 9:45
- implemented binary record handling and function table.

# Time: 9:53
- resolve and patch function call destinations
- Pass 0x1: Resolve Done

# Date 11 Oct 2026
# Time 11:08
- wrote tokenline func which separates an instruction into words.
- make check of space handling, blank lines, full-line comments, and token capacity.
- Execution of the tokens remains pending.

# Time 11:40
- executed set execusion by implementing 3 helper functions, findVaraible, getValue, execute program.
- now implement , add, sub, mul and div using these same helper funcs.

# Time 12:30
- Implemented add, sub, mul, and div using 32-bit integers.
- Added checks for instruction format, unsupported operations, missing destination variables, and division by zero.
- Implementation assumption: all arithmetic results fit within the signed 32-bit range. 
- Next: connect instruction execution to resolve.bin and the call stack.

# Date: 12 Oct 2026

# Time 12:40 AM 
- doing work on raat ke is time :(. so much dedication.
- parameter binding implentation done.

# Time 1:20 
- Connected validation, resolution, and execution in a separate check program.
- Execute function done.
- Next: implement the doubly linked timeline and record independent execution snapshots.

# Time 10:44
- build timeline class
- next: build snapshot func

# Time 11:18
- build snapshot func written 0x2 is completed

# Time 11:58
- 0x3 completed