import socket
import sys

def send_python_command(host: str, port: int, code: str) -> str:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.connect((host, port))
        message = code + "\n---EXEC---\n"
        sock.sendall(message.encode('utf-8'))
        response = sock.recv(4096).decode('utf-8')
        return response

if __name__ == "__main__":
    host = "127.0.0.1"
    port = 5555
    
    if len(sys.argv) > 1:
        code = sys.argv[1]
    else:
        code = 'print("Hello from TCP!")'
    
    try:
        result = send_python_command(host, port, code)
        print(f"Response: {result}")
    except ConnectionRefusedError:
        print(f"Error: Could not connect to {host}:{port}")
        print("Make sure Ruzino is running with the TCP server enabled.")
    except Exception as e:
        print(f"Error: {e}")
