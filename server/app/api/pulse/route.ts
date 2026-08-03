import { deviceAuthorised, webhookValid } from '@/lib/auth'
import { bump, current } from '@/lib/store'

export const dynamic = 'force-dynamic'

//the device hits this every ~10s. keep it cheap and boring.
export async function GET(req: Request) {
  if (!deviceAuthorised(req)) {
    return Response.json({ error: 'unauthorised' }, { status: 401 })
  }

  const { seq, at, durable } = await current()
  return Response.json({ seq, at, durable })
}

/**
 * Two callers land here:
 *
 *  1. a github webhook (push event), signed with WEBHOOK_SECRET
 *  2. a local pre-push hook, which just presents the device token
 *
 * Both do the same thing: nudge the counter so the display flashes.
 */
export async function POST(req: Request) {
  const raw = await req.text()
  const sig = req.headers.get('x-hub-signature-256')

  const fromGitHub = sig !== null && webhookValid(raw, sig)
  const fromHook = deviceAuthorised(req)

  if (!fromGitHub && !fromHook) {
    return Response.json({ error: 'unauthorised' }, { status: 401 })
  }

  //ignore everything that isn't a push, otherwise stars and issue comments set
  //the thing flashing all day. keyed off the header rather than the auth path,
  //so this still holds if a webhook ever reaches us some other way.
  const event = req.headers.get('x-github-event')
  if (event) {
    if (event === 'ping') return Response.json({ ok: true, pong: true })
    if (event !== 'push') return Response.json({ ok: true, ignored: event })
  }

  const seq = await bump()

  return Response.json({ ok: true, seq })
}
