# One self-contained HTML page for one image or for the diff of two
# (tools/checkpoint_inspect.rb --html, diff --html). Nothing is loaded from
# outside: the inspect JSON is embedded in a <script type="application/json">
# element, and the page (inspect_html/app.js, inspect_html/style.css) draws
# it with plain DOM and SVG.

require 'json'

module CheckpointHtml
  DIR = File.join(__dir__, 'inspect_html')
  DATA_ID = 'checkpoint-data'

  module_function

  # the JSON as it is embedded: "</" cannot end the script element
  def embed(obj) = JSON.generate(obj).gsub('</', '<\/')

  # the JSON back out of a page written by `page` (for the tests and for
  # tools that take the page as their input)
  def extract(html)
    m = html.match(%r{<script type="application/json" id="#{DATA_ID}">(.*?)</script>}m) or return nil
    JSON.parse(m[1].gsub('<\/', '</'))
  end

  def escape(s) = s.to_s.gsub('&', '&amp;').gsub('<', '&lt;').gsub('>', '&gt;').gsub('"', '&quot;')

  def page(docs, diff)
    title = diff ? "像の差: #{docs[0]['source']['file']} → #{docs[1]['source']['file']}" : "像: #{docs[0]['source']['file']}"
    data = { 'docs' => docs, 'diff' => diff }
    <<~HTML
      <!doctype html>
      <html lang="ja">
      <head>
      <meta charset="utf-8">
      <meta name="viewport" content="width=device-width, initial-scale=1">
      <title>#{escape(title)}</title>
      <style>
      #{File.read(File.join(DIR, 'style.css'))}
      </style>
      </head>
      <body>
      <header id="top"><h1>#{escape(title)}</h1><div id="summary"></div><nav id="tabs"></nav></header>
      <main id="main"><noscript>この表示には JavaScript が要る。中身は下の JSON にある。</noscript></main>
      <script type="application/json" id="#{DATA_ID}">#{embed(data)}</script>
      <script>
      #{File.read(File.join(DIR, 'app.js'))}
      </script>
      </body>
      </html>
    HTML
  end
end
