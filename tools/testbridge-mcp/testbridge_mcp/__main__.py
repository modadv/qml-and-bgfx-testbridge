import argparse
import logging

from .server import run_stdio


def main() -> None:
    parser = argparse.ArgumentParser(description="TestBridge MCP server")
    parser.add_argument("--host", default="127.0.0.1", help="TestBridge WebSocket host")
    parser.add_argument("--port", type=int, default=47600, help="TestBridge WebSocket port")
    parser.add_argument("--log-level", default="INFO", help="Logging level")
    args = parser.parse_args()

    logging.basicConfig(level=getattr(logging, args.log_level.upper(), logging.INFO))

    run_stdio(host=args.host, port=args.port)


if __name__ == "__main__":
    main()
