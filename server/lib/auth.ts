import { createHmac, timingSafeEqual } from 'node:crypto'

//assume anything on the device is readable by whoever holds the device.
//this token guards read-only counters, nothing more, and it is deliberately
//separate from any other credential you own so it can be rotated on its own.
export function deviceAuthorised(req: Request): boolean {
  const want = process.env.DEVICE_TOKEN
  if (!want) return false //fail closed when unconfigured

  const got = req.headers.get('x-device-token') ?? ''
  const a = Buffer.from(got)
  const b = Buffer.from(want)

  if (a.length !== b.length) return false

  return timingSafeEqual(a, b)
}

//github signs webhook bodies with sha256. verify before trusting a push.
export function webhookValid(raw: string, signature: string | null): boolean {
  const secret = process.env.WEBHOOK_SECRET
  if (!secret || !signature) return false

  const mac = 'sha256=' + createHmac('sha256', secret).update(raw).digest('hex')
  const a = Buffer.from(mac)
  const b = Buffer.from(signature)

  if (a.length !== b.length) return false

  return timingSafeEqual(a, b)
}
