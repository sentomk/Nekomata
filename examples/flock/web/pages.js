// Static-hosting flavor of the page. Every generation on the tour was compiled
// ahead of time; choosing one here only decides which manifest the page's
// poller sees next. Fetching the artifact, checking its SHA-256 and ABI
// identity, and switching code at the frame boundary are Nekomata's own work.
(async () => {
  const generations = await (await fetch('generations.json')).json();
  for (const generation of generations) {
    generation.id = generation.manifest.match(/^generation_id "([^"]*)"/m)[1];
  }

  // The backend polls offers/latest through XMLHttpRequest. Answer with the
  // manifest published last, under a fresh sequence number so that any order
  // of choices supersedes the one before it.
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

  const tour = document.createElement('aside');
  tour.className = 'tour';
  tour.innerHTML = `
    <h2>Generations</h2>
    <p class="intro">Changes to <code>flock.cpp</code>, compiled ahead of time.
    Pick one and the running page switches to it at its next frame.</p>
    <ol></ol>
    <div class="detail"><p></p><pre><code></code></pre></div>
    <p class="source"><a href="https://github.com/sentomk/Nekomata/tree/main/examples/flock">Source</a></p>`;
  const list = tour.querySelector('ol');
  const items = new Map();

  const focus = (generation) => {
    for (const [id, item] of items) {
      item.classList.toggle('focused', id === generation.id);
    }
    tour.querySelector('.detail p').textContent = generation.summary;
    tour.querySelector('.detail code').textContent = generation.snippet;
  };

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
    // A choice still waiting is superseded by this one and will never be reported.
    for (const item of items.values()) {
      if (item.dataset.state === 'waiting') {
        item.dataset.state = '';
      }
    }
    items.get(generation.id).dataset.state = 'waiting';
    focus(generation);
  };

  for (const generation of generations) {
    const item = document.createElement('li');
    item.innerHTML = '<button type="button"><span class="title"></span><span class="note"></span></button>';
    item.querySelector('.title').textContent = generation.title;
    item.querySelector('button').addEventListener('click', () => publish(generation));
    items.set(generation.id, item);
    list.append(item);
  }

  const layout = document.querySelector('.layout');
  layout.classList.add('with-tour');
  layout.append(tour);

  // Mark each item with the page's own verdict on it.
  const report = window.flock_event;
  window.flock_event = (event) => {
    report(event);
    const item = items.get(event.generation);
    if (!item) {
      return;
    }
    if (event.applied) {
      for (const other of items.values()) {
        if (other.dataset.state === 'running') {
          other.dataset.state = '';
        }
      }
      item.dataset.state = 'running';
      item.querySelector('.note').textContent = '';
    } else {
      item.dataset.state = 'rejected';
      item.querySelector('.note').textContent = event.code;
    }
  };

  publish(generations[0]);
  const script = document.createElement('script');
  script.src = 'main.js';
  document.body.append(script);
})();
