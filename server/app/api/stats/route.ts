import { deviceAuthorised } from '@/lib/auth'
import { pack, resolveAdapters, type Snapshot } from '@/lib/adapters'

//the device polls far more often than this, and the contribution graph does not
//move fast enough to justify passing every one of those through to github.
export const revalidate = 30

let cached: { at: number; body: unknown } | null = null

export async function GET(req: Request) {
  if (!deviceAuthorised(req)) {
    return Response.json({ error: 'unauthorised' }, { status: 401 })
  }

  const fresh = cached && Date.now() - cached.at < revalidate * 1000
  if (fresh) {
    return Response.json(cached!.body, { headers: { 'x-cache': 'hit' } })
  }

  const ids = (process.env.ADAPTERS ?? 'github').split(',').map((s) => s.trim())
  const adapters = await resolveAdapters(ids)

  const panels: Record<string, unknown> = {}
  const failed: string[] = []

  for (const a of adapters) {
    try {
      const snap: Snapshot = await a.load()
      panels[a.id] = { label: a.label, ...pack(snap) }
    } catch (err) {
      //one bad adapter shouldn't blank the whole display
      failed.push(a.id)
      console.error(`adapter ${a.id} failed:`, err)
    }
  }

  if (!Object.keys(panels).length) {
    return Response.json({ error: 'all adapters failed', failed }, { status: 502 })
  }

  const body = { panels, failed, at: Date.now() }
  cached = { at: Date.now(), body }

  return Response.json(body, { headers: { 'x-cache': 'miss' } })
}
