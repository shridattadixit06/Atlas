
import socket

HOST = "127.0.0.1"
PORT = 8080


def recv_line(sock):
    data = bytearray()

    while True:
        byte = sock.recv(1)

        if not byte:
            raise ConnectionError("Server closed the connection")

        if byte == b"\n":
            return data.decode("ascii")

        data.extend(byte)


def recv_exact(sock, length):
    data = bytearray()

    while len(data) < length:
        chunk = sock.recv(length - len(data))

        if not chunk:
            raise ConnectionError("Incomplete file response")

        data.extend(chunk)

    return bytes(data)


def write_file(name, content):
    with socket.create_connection((HOST, PORT)) as sock:
        header = f"WRITE {name} {len(content)}\n".encode("ascii")
        sock.sendall(header + content)
        print("WRITE:", recv_line(sock))


def read_file(name):
    with socket.create_connection((HOST, PORT)) as sock:
        sock.sendall(f"READ {name}\n".encode("ascii"))

        response = recv_line(sock)

        if not response.startswith("OK "):
            print("READ error:", response)
            return None

        length = int(response.split()[1])
        content = recv_exact(sock, length)

        print("READ:", repr(content))
        return content


if __name__ == "__main__":
    original = b"Atlas is learning distributed storage!\n"

    write_file("test.txt", original)
    recovered = read_file("test.txt")

    assert recovered == original, "File contents do not match!"
    print("PASS: Written and read contents match.")
