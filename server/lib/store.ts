/**
 * Pulse storage.
 *
 * The device polls a counter and flashes green when it goes up. Something has to
 * remember that number between a webhook POST and the next device GET.
 *
 * Serverless caveat worth being blunt about: the in-memory fallback below does NOT
 * work reliably in production. Each Vercel instance gets its own module scope, so a
 * webhook handled by instance A is invisible to a poll served by instance B, and
 * everything resets on cold start. It is fine for `next dev` and nothing else.
 *
 * Set UPSTASH_REDIS_REST_URL + UPSTASH_REDIS_REST_TOKEN (Upstash is on the Vercel
 * marketplace, free tier is plenty for a counter) and this uses that instead.
 */

const KEY = 'esp-git:seq'

let memory = { seq: 0, at: 0 }

function upstash() {
  const url = process.env.UPSTASH_REDIS_REST_URL
  const token = process.env.UPSTASH_REDIS_REST_TOKEN
  if (!url || !token) return null

  return async (...cmd: (string | number)[]) => {
    const res = await fetch(`${url}/${cmd.map(encodeURIComponent).join('/')}`, {
      headers: { authorization: `Bearer ${token}` },
      cache: 'no-store',
    })
    if (!res.ok) throw new Error(`upstash ${res.status}`)
    return (await res.json()).result
  }
}

export async function bump(): Promise<number> {
  const redis = upstash()

  if (redis) {
    const seq = Number(await redis('INCR', KEY))
    await redis('SET', `${KEY}:at`, Date.now())
    return seq
  }

  memory = { seq: memory.seq + 1, at: Date.now() }
  return memory.seq
}

export async function current(): Promise<{ seq: number; at: number; durable: boolean }> {
  const redis = upstash()

  if (redis) {
    const [seq, at] = await Promise.all([redis('GET', KEY), redis('GET', `${KEY}:at`)])
    return { seq: Number(seq ?? 0), at: Number(at ?? 0), durable: true }
  }

  return { ...memory, durable: false }
}

/* ------------------------------------------------------ remote commands */

const QUEUE = 'esp-git:cmds'
const REPORT = 'esp-git:report'

let memQueue: string[] = []
let memReport = { text: '', at: 0 }

export async function pushCommand(cmd: string): Promise<number> {
  const redis = upstash()
  if (redis) return Number(await redis('RPUSH', QUEUE, cmd))

  memQueue.push(cmd)
  return memQueue.length
}

export async function popCommand(): Promise<string | null> {
  const redis = upstash()
  if (redis) return (await redis('LPOP', QUEUE)) ?? null

  return memQueue.shift() ?? null
}

export async function setReport(text: string) {
  const redis = upstash()
  if (redis) {
    await redis('SET', REPORT, text)
    await redis('SET', `${REPORT}:at`, Date.now())
    return
  }
  memReport = { text, at: Date.now() }
}

export async function lastReport() {
  const redis = upstash()
  if (redis) {
    const [text, at] = await Promise.all([redis('GET', REPORT), redis('GET', `${REPORT}:at`)])
    return { text: text ?? '', at: Number(at ?? 0) }
  }
  return memReport
}
