// Static-hosting flavor of the page. Every generation on the tour was compiled
// ahead of time; publishing one here only decides which manifest the page's
// poller sees next. Fetching the artifact, checking its SHA-256 and ABI
// identity, and switching code at the frame boundary are Nekomata's own work.
(async () => {
  const generations = await (await fetch('generations.json')).json();
  const field = (manifest, name) => (manifest.match(new RegExp(`^${name} "([^"]*)"`, 'm')) || [])[1];
  for (const generation of generations) {
    generation.generation_id = field(generation.manifest, 'generation_id');
  }

  // The backend polls offers/latest through XMLHttpRequest. Answer with the
  // manifest published last, under a fresh sequence number so that any order
  // of publishes supersedes the one before it.
  let sequence = 0;
  let latest = null;
  const retired = [];
  const open = XMLHttpRequest.prototype.open;
  XMLHttpRequest.prototype.open = function (method, url, ...rest) {
    if (latest && new URL(url, document.baseURI).pathname.endsWith('/offers/latest')) {
      url = latest;
    }
    return open.call(this, method, url, ...rest);
  };

  const cards = new Map();
  const publish = (generation) => {
    sequence += 1;
    const text = generation.manifest.replace(/^sequence \d+$/m, `sequence ${sequence}`);
    if (latest) {
      retired.push(latest);
    }
    // Keep recent URLs alive for polls that are already in flight.
    while (retired.length > 2) {
      URL.revokeObjectURL(retired.shift());
    }
    latest = URL.createObjectURL(new Blob([text], { type: 'text/plain' }));
    const card = cards.get(generation.generation_id);
    if (card) {
      card.dataset.state = 'pending';
      card.querySelector('.status').textContent = 'published — waiting for the next poll';
    }
  };

  const panel = document.createElement('aside');
  panel.className = 'tour';
  panel.innerHTML = `
    <h2>Publish a generation</h2>
    <p class="intro">Each card is a change to <code>flock.cpp</code>, compiled ahead of time
    because this page is static. Publishing hands the running page its manifest; the page
    verifies the module and switches at its next frame without touching the world.
    To hot-reload your own edits, see
    <a href="https://github.com/sentomk/Nekomata">Nekomata on GitHub</a>.</p>
    <div class="cards"></div>`;
  const list = panel.querySelector('.cards');
  generations.forEach((generation, index) => {
    const card = document.createElement('article');
    card.className = 'card';
    card.dataset.state = 'idle';
    card.innerHTML = `
      <header>
        <span class="step">${index + 1}</span><h3></h3>
        <button type="button">Publish</button>
      </header>
      <p class="summary"></p>
      <pre><code></code></pre>
      <p class="status"></p>`;
    card.querySelector('h3').textContent = generation.title;
    const summary = card.querySelector('.summary');
    summary.textContent = generation.summary;
    if (generation.expect !== 'applied') {
      const expectation = document.createElement('span');
      expectation.className = 'expect';
      expectation.textContent = ` Expected: rejected (${generation.expect}).`;
      summary.append(expectation);
    }
    card.querySelector('code').textContent = generation.snippet;
    card.querySelector('button').addEventListener('click', () => publish(generation));
    cards.set(generation.generation_id, card);
    list.append(card);
  });
  const layout = document.querySelector('.layout');
  layout.classList.add('with-tour');
  layout.append(panel);

  // Reflect the page's own reload events on the cards.
  const log = window.flock_log;
  window.flock_log = (ok, text) => {
    log(ok, text);
    const match = text.match(/^(applied|rejected) generation (\S+)(?: \((\w+)\))?/);
    const card = match && cards.get(match[2]);
    if (!card) {
      return;
    }
    if (match[1] === 'applied') {
      for (const other of cards.values()) {
        if (other.dataset.state === 'running') {
          other.dataset.state = 'idle';
          other.querySelector('.status').textContent = '';
        }
      }
      card.dataset.state = 'running';
      card.querySelector('.status').textContent = 'running';
    } else {
      card.dataset.state = 'rejected';
      card.querySelector('.status').textContent = `rejected (${match[3]}) — previous code kept`;
    }
  };

  publish(generations[0]);
  const script = document.createElement('script');
  script.src = 'main.js';
  document.body.append(script);
})();
