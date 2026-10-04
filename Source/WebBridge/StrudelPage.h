#pragma once

namespace WebBridge
{

// Host page for the locally served Strudel REPL. The Strudel build itself
// (the package's dist/ folder) is fetched from npm at runtime, see StrudelFetch.h.
inline const char* getStrudelPage()
{
    return R"HTML(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>Strudel (offline)</title>
<style>
  html, body { margin: 0; min-height: 100%; background: #222; color: #eee; }
  strudel-editor { display: none; }   /* editor is inserted as the next sibling */
  .cm-editor { min-height: 100vh; }
</style>
</head>
<body>
<strudel-editor>
<!--
silence
-->
</strudel-editor>
<script src="index.js"></script>
<script>
  // Bridge script looks for window.strudelMirror (code persistence, DAW sync).
  // Once the editor exists, ask the plugin for the code saved in the DAW project.
  const el = document.querySelector('strudel-editor');
  const wait = setInterval(() => {
    if (!el.editor) return;
    clearInterval(wait);
    window.strudelMirror = el.editor;
    window.__JUCE__?.backend?.emitEvent('requestCode', {});
  }, 100);
</script>
</body>
</html>
)HTML";
}

// Served in place of the REPL page while the Strudel build is not on disk.
inline const char* getStrudelMissingPage()
{
    return R"HTML(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>Strudel not installed</title>
<style>
  html, body { margin: 0; min-height: 100%; background: #222; color: #eee;
               font: 14px/1.5 -apple-system, system-ui, sans-serif; }
  main { max-width: 520px; margin: 18vh auto 0; padding: 0 24px; }
  h1 { font-size: 18px; color: #ffb703; margin: 0 0 12px; }
  p { margin: 0 0 10px; color: #c8ccd6; }
  code { background: #333; padding: 1px 5px; border-radius: 3px; color: #6ee7b7; }
</style>
</head>
<body>
<main>
  <h1>Strudel is not installed</h1>
  <p>The local Strudel build has not been downloaded yet, so this page cannot run.</p>
  <p>Open settings (<code>&#9881;</code>, top right of the plugin) and press <code>&#8595;</code> to fetch
     the latest <code>@strudel/repl</code> from npm. It is stored once and works offline afterwards.</p>
  <p>Strudel is AGPL and is not bundled with the plugin.</p>
</main>
</body>
</html>
)HTML";
}

}
