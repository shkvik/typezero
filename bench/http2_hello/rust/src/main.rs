// The server of the HTTP/2 benchmark (bench/http2.ps1): "Hello, World!" to
// every request over HTTP/2 without TLS (h2c, prior knowledge), with hyper,
// on the port given, as main.tz.
use bytes::Bytes;
use http_body_util::Full;
use hyper::server::conn::http2;
use hyper::service::service_fn;
use hyper::{Request, Response};
use hyper_util::rt::{TokioExecutor, TokioIo};
use std::convert::Infallible;
use tokio::net::TcpListener;

async fn hello(_: Request<hyper::body::Incoming>) -> Result<Response<Full<Bytes>>, Infallible> {
    let mut response = Response::new(Full::new(Bytes::from_static(b"Hello, World!")));
    response
        .headers_mut()
        .insert("content-type", "text/plain;charset=UTF-8".parse().unwrap());
    Ok(response)
}

#[tokio::main]
async fn main() {
    let port = std::env::args().nth(1).unwrap_or_else(|| "8080".to_string());
    let listener = TcpListener::bind(format!("127.0.0.1:{port}")).await.unwrap();
    loop {
        let (stream, _) = listener.accept().await.unwrap();
        stream.set_nodelay(true).unwrap();
        tokio::spawn(async move {
            let _ = http2::Builder::new(TokioExecutor::new())
                .serve_connection(TokioIo::new(stream), service_fn(hello))
                .await;
        });
    }
}
