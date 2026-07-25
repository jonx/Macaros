//! host-lsp-bridge — expose a stdio language server on a loopback TCP port.
//!
//! `rust-analyzer` (and most language servers) speak LSP over stdin/stdout.
//! AROS can't spawn a host subprocess, but it can open a TCP socket to the
//! Mac's loopback through the bsdsocket bridge. This tool runs on the host
//! (macOS), spawns the language server, and pumps bytes between its stdio
//! and a TCP connection. LSP framing (Content-Length headers) is identical
//! on both sides, so this is a transparent byte pump — no parsing.
//!
//! This is the standalone form of the "embed the language server in the
//! Macaros host" design: the same pump logic later moves into the cocoametal
//! host so the server starts with AROS automatically.
//!
//!   host-lsp-bridge [--port N] [--] <server> [server args...]
//!   host-lsp-bridge --port 9257 -- rust-analyzer
//!
//! One client at a time: each accepted connection spawns a fresh server and
//! tears it down when the socket closes (the LSP model is one server per
//! session). Bind is 127.0.0.1 only — never exposed off-host.

use std::io::{Read, Write};
use std::net::{Ipv4Addr, TcpListener, TcpStream};
use std::process::{Child, Command, Stdio};
use std::thread;

const DEFAULT_PORT: u16 = 9257;

fn main() {
    let mut args = std::env::args().skip(1).peekable();
    let mut port = DEFAULT_PORT;
    let mut server: Vec<String> = Vec::new();
    while let Some(a) = args.next() {
        match a.as_str() {
            "--port" => {
                port = args
                    .next()
                    .and_then(|p| p.parse().ok())
                    .unwrap_or_else(|| fatal("--port needs a number"));
            }
            "--" => {
                server.extend(args.by_ref());
            }
            other => server.push(other.to_string()),
        }
    }
    if server.is_empty() {
        server.push("rust-analyzer".to_string());
    }

    let listener = TcpListener::bind((Ipv4Addr::LOCALHOST, port))
        .unwrap_or_else(|e| fatal(&format!("bind 127.0.0.1:{port}: {e}")));
    eprintln!(
        "[lsp-bridge] listening on 127.0.0.1:{port}, server = {}",
        server.join(" ")
    );

    for conn in listener.incoming() {
        let stream = match conn {
            Ok(s) => s,
            Err(e) => {
                eprintln!("[lsp-bridge] accept error: {e}");
                continue;
            }
        };
        let peer = stream.peer_addr().map(|a| a.to_string()).unwrap_or_default();
        eprintln!("[lsp-bridge] client {peer} connected; starting server");
        // One thread per client so a wedged/half-closed connection can never
        // block accepting the next one (each session owns its own server).
        let server = server.clone();
        thread::spawn(move || {
            if let Err(e) = serve(&server, stream) {
                eprintln!("[lsp-bridge] client {peer} session ended: {e}");
            } else {
                eprintln!("[lsp-bridge] client {peer} disconnected");
            }
        });
    }
}

/// Spawn the server and pump bytes both ways until either side closes.
fn serve(server: &[String], stream: TcpStream) -> std::io::Result<()> {
    let mut child: Child = Command::new(&server[0])
        .args(&server[1..])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::inherit())
        .spawn()?;

    let mut child_stdin = child.stdin.take().expect("piped stdin");
    let mut child_stdout = child.stdout.take().expect("piped stdout");
    let mut sock_read = stream.try_clone()?;
    let mut sock_write = stream;

    // socket -> server stdin. Flush after each chunk so the server sees the
    // request promptly (it drives when it replies, which the AROS client is
    // parked waiting for).
    let up = thread::spawn(move || {
        let mut buf = [0u8; 16 * 1024];
        loop {
            match sock_read.read(&mut buf) {
                Ok(0) | Err(_) => break,
                Ok(n) => {
                    if child_stdin
                        .write_all(&buf[..n])
                        .and_then(|_| child_stdin.flush())
                        .is_err()
                    {
                        break;
                    }
                }
            }
        }
        // The client is gone (clean quit or a hard AROS kill). Drive a graceful
        // LSP shutdown on its behalf so the server exits code 0 instead of
        // erroring with "client exited without proper shutdown sequence".
        let _ = lsp_frame(&mut child_stdin, r#"{"jsonrpc":"2.0","id":2147483647,"method":"shutdown"}"#);
        let _ = lsp_frame(&mut child_stdin, r#"{"jsonrpc":"2.0","method":"exit"}"#);
        // Dropping child_stdin closes it, backstopping servers that ignore exit.
    });
    // server stdout -> socket. Stop on either EOF or a broken socket.
    let mut buf = [0u8; 16 * 1024];
    loop {
        match child_stdout.read(&mut buf) {
            Ok(0) | Err(_) => break,
            Ok(n) => {
                if sock_write.write_all(&buf[..n]).is_err() {
                    break;
                }
            }
        }
    }
    // The server exits on its own after the injected `exit`; reap it (and its
    // stdin-writer thread). No SIGKILL — that's what caused the ungraceful exit.
    let _ = up.join();
    let _ = child.wait();
    Ok(())
}

/// Write one LSP message: `Content-Length: N\r\n\r\n<body>`.
fn lsp_frame(w: &mut impl Write, body: &str) -> std::io::Result<()> {
    write!(w, "Content-Length: {}\r\n\r\n{body}", body.len())?;
    w.flush()
}

fn fatal(msg: &str) -> ! {
    eprintln!("[lsp-bridge] fatal: {msg}");
    std::process::exit(1);
}
