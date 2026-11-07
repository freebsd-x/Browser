# Browser

Browser — a web browser for FreeBSD-X.

Pages open in FreeBSD-X's own web engine, [xweb](https://github.com/freebsd-x/xweb),
sandboxed like the other built-in apps. What it can't do yet (JavaScript, forms,
cookies) is one click away: **Open in Chromium** hands the page to Chromium, which runs
through the Wayland bridge if it's installed (`pkg install chromium`).

Part of **FreeBSD-X**, a GUI platform for FreeBSD. The full workspace is a
multi-repo project assembled with the [`repo`](https://gerrit.googlesource.com/git-repo)
tool:

```sh
repo init -u https://github.com/freebsd-x/manifest.git
repo sync
```

## License

Released under the MIT License — see [LICENSE](LICENSE).
