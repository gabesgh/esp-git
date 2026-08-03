import { deviceAuthorised } from '@/lib/auth'
import { popCommand, pushCommand, lastReport, setReport } from '@/lib/store'

export const dynamic = 'force-dynamic'

/**
 * The device sits behind NAT, so nothing can reach it. It reaches out instead:
 * polls this on the same tick as the pulse, runs whatever it finds, and posts
 * the output back. That inverts the direction of travel and means remote control
 * needs no port forwarding, no static address and no tunnel.
 *
 * GET  (device)  pops one queued command
 * POST (you)     queues a command, or accepts a report back from the device
 */
export async function GET(req: Request) {
  if (!deviceAuthorised(req)) {
    return Response.json({ error: 'unauthorised' }, { status: 401 })
  }

  const url = new URL(req.url)

  //asking for the last thing the device said rather than for work to do
  if (url.searchParams.has('report')) {
    return Response.json(await lastReport())
  }

  const cmd = await popCommand()
  return Response.json(cmd ? { cmd } : {})
}

export async function POST(req: Request) {
  if (!deviceAuthorised(req)) {
    return Response.json({ error: 'unauthorised' }, { status: 401 })
  }

  const body = await req.json().catch(() => ({}))

  if (typeof body.report === 'string') {
    await setReport(body.report)
    return Response.json({ ok: true })
  }

  if (typeof body.cmd !== 'string' || !body.cmd.length) {
    return Response.json({ error: 'need cmd or report' }, { status: 400 })
  }

  const depth = await pushCommand(body.cmd.slice(0, 64))
  return Response.json({ ok: true, queued: body.cmd, depth })
}
