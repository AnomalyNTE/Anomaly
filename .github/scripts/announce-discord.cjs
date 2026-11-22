const { readFile } = require('node:fs/promises');
const { setTimeout: delay } = require('node:timers/promises');

const descriptionLimit = 4096;
const logoUrl = 'https://raw.githubusercontent.com/AnomalyNTE/Anomaly/main/logo.png';

function splitNotes(notes) {
  const lines = notes.split('\n');
  // A line that cannot fit in any embed is sent intact in the attachment.
  if (lines.some(line => line.length > descriptionLimit)) return null;

  const parts = [];
  let current = [];
  let length = 0;
  for (const line of lines) {
    const nextLength = length + (current.length ? 1 : 0) + line.length;
    if (nextLength > descriptionLimit) {
      parts.push(current.join('\n'));
      current = [];
      length = 0;
    }
    length += (current.length ? 1 : 0) + line.length;
    current.push(line);
  }
  parts.push(current.join('\n'));
  return parts;
}

async function sendWebhook(url, payload, attachment) {
  let request;
  if (attachment !== undefined) {
    const form = new FormData();
    form.set('payload_json', JSON.stringify(payload));
    form.set('files[0]', new Blob([attachment], { type: 'text/markdown;charset=utf-8' }), 'CHANGES.md');
    request = { body: form };
  } else {
    request = { headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload) };
  }

  for (let attempt = 0; ; attempt++) {
    const response = await fetch(url, { method: 'POST', ...request, signal: AbortSignal.timeout(30_000) });
    if (response.status === 429 && attempt < 3) {
      const { retry_after: retryAfter } = await response.json();
      await delay(Math.ceil(retryAfter * 1000));
      continue;
    }
    if (!response.ok) throw new Error(`Discord webhook failed: HTTP ${response.status}.`);
    await response.arrayBuffer();
    return response.headers;
  }
}

module.exports = async function announceDiscordRelease() {
  const originalNotes = await readFile('CHANGES.md', 'utf8');
  const notes = originalNotes.replace(/\r/g, '').replace(/<!--.*?-->/gs, '').trim();
  if (!notes) throw new Error('No release notes to send to Discord.');

  const parts = splitNotes(notes);
  const descriptions = parts ?? ['更新日志中有单行超过 Discord 的长度限制，请查看附件 CHANGES.md 或点击标题阅读完整内容。'];
  const url = new URL(process.env.DISCORD_WEBHOOK_URL);
  url.searchParams.set('wait', 'true');
  const footerTitle = process.env.DISCORD_FOOTER_TITLE || 'Anomaly';
  const timestamp = new Date().toISOString();

  for (const [index, description] of descriptions.entries()) {
    const payload = {
      username: 'Anomaly',
      avatar_url: logoUrl,
      allowed_mentions: { parse: [] },
      content: index === 0 ? (process.env.DISCORD_CONTENT || '') : '',
      embeds: [{
        title: process.env.DISCORD_RELEASE_NAME.slice(0, 256).replace(/[\uD800-\uDBFF]$/, ''),
        url: process.env.DISCORD_RELEASE_URL,
        color: 11052455,
        description,
        footer: {
          text: descriptions.length > 1 ? `${footerTitle} (${index + 1}/${descriptions.length})` : footerTitle,
          icon_url: logoUrl,
        },
        timestamp,
      }],
    };
    const headers = await sendWebhook(url, payload, parts === null ? originalNotes : undefined);
    console.log(`Sent Discord release notes (${index + 1}/${descriptions.length}).`);
    if (index + 1 < descriptions.length && headers.get('x-ratelimit-remaining') === '0') {
      await delay(Math.ceil(Number(headers.get('x-ratelimit-reset-after')) * 1000));
    }
  }
};
