import socket
s = socket.socket()
s.bind(('0.0.0.0', 8080))
s.listen(1)
print("Waiting for ESP...")
c, addr = s.accept()
print(f"Connected: {addr}")
while True:
    data = c.recv(1024)
    if not data:
        break
    print(data.decode(), end='')
