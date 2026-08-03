import { readFile } from 'node:fs/promises'
import { join } from 'node:path'
import { deviceAuthorised } from '@/lib/auth'

export const dynamic = 'force-dynamic'

const DIR = join(process.cwd(), "public", "firmware")

//HTTPUpdate on the device sends Authorization rather than our own header, so
//accept either. Same token, same constant-time check.
function allowed(req: Request) {
  if (deviceAuthorised(req)) return true

  const auth = req.headers.get('authorization') ?? ''
  if (!auth.startsWith('Bearer ')) return false

  return deviceAuthorised(
    new Request(req.url, { headers: { 'x-device-token': auth.slice(7) } }),
  )
}

export async function GET(req: Request) {
  if (!allowed(req)) {
    return Response.json({ error: 'unauthorised' }, { status: 401 })
  }

  let manifest: { version: string; size: number; notes?: string }
  try {
    manifest = JSON.parse(await readFile(join(DIR, 'manifest.json'), 'utf8'))
  } catch {
    return Response.json({ error: 'no release published' }, { status: 404 })
  }

  //metadata probe. the device does this first and only downloads if the version
  //actually moved, which keeps a 1MB transfer off the wifi every poll.
  if (new URL(req.url).searchParams.has('meta')) {
    return Response.json(manifest)
  }

  const bin = await readFile(join(DIR, 'firmware.bin'))
  const body = new Uint8Array(bin)

  return new Response(body, {
    headers: {
      'content-type': 'application/octet-stream',
      'content-length': String(body.byteLength),
      'x-firmware-version': manifest.version,
    },
  })
}
