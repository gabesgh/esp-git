import type { NextConfig } from 'next'

const config: NextConfig = {
  // the firmware image is read at request time, so it has to be traced into the
  // serverless bundle or the route 404s in production while working locally.
  outputFileTracingIncludes: {
    '/api/firmware': ['./public/firmware/**'],
  },
}

export default config
