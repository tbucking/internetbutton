// ingest-activity
//
// Receives a Particle webhook forward and writes aggregated activity buckets into
// public.activity_buckets. See docs/ACTIVITY.md ("The cloud end") for the wire
// format and rationale.
//
// The webhook posts JSON:
//
//   { event, data, coreid, published_at }
//
// where `data` is the firmware's compact CSV:
//
//   v1,<startMin>,<n>,<count>,<sum>,<maxDelta>,<maxDz>;...
//
// `startMin` is unix minutes at the start of the first bucket. Buckets are
// consecutive minutes, so bucket `i` starts at `startMin + i`. The single header
// timestamp means a firmware clock jump that skips minutes would shift the
// following buckets in the same event; that is a known, accepted limitation.

import { createClient } from 'jsr:@supabase/supabase-js@2'

// Auto-injected by the platform. The service-role key bypasses RLS, which is how
// the function writes even though activity_buckets has RLS enabled with no
// policies.
const supabase = createClient(
  Deno.env.get('SUPABASE_URL')!,
  Deno.env.get('SUPABASE_SERVICE_ROLE_KEY')!,
)

interface Bucket {
  device_id: string
  bucket_start: string
  impulses: number
  impulse_sum: number
  max_delta: number | null
  max_dz: number | null
  batch: number
}

function parsePayload(coreid: string, data: string): Bucket[] {
  const m = /^v1,(\d+),(\d+),(.*)$/s.exec(data)
  if (!m) {
    throw new Error('unrecognized payload version')
  }
  const startMin = Number(m[1])
  const n = Number(m[2])
  const rest = m[3]

  const buckets: Bucket[] = []
  for (const field of rest.split(';')) {
    if (field === '') continue
    const parts = field.split(',')
    if (parts.length !== 4) throw new Error('malformed bucket record')

    const count = Number(parts[0])
    const sum = Number(parts[1])
    const maxDelta = Number(parts[2])
    const maxDz = Number(parts[3])
    const batch = buckets.length

    buckets.push({
      device_id: coreid,
      bucket_start: new Date((startMin + batch) * 60 * 1000).toISOString(),
      impulses: count,
      impulse_sum: sum,
      // The firmware sends 0 to mean "nothing to record" (see docs/ACTIVITY.md),
      // which is stored as NULL rather than a misleading zero.
      max_delta: maxDelta === 0 ? null : maxDelta,
      max_dz: maxDz === 0 ? null : maxDz,
      batch,
    })
  }

  // `n` is the firmware's intended count; the parsed records are authoritative,
  // but a mismatch means the payload was truncated or corrupted.
  if (buckets.length !== n) {
    throw new Error(`expected ${n} buckets, parsed ${buckets.length}`)
  }

  return buckets
}

Deno.serve(async (req) => {
  if (req.method !== 'POST') {
    return new Response('method not allowed', { status: 405 })
  }

  let body: { coreid?: string; data?: string }
  try {
    body = await req.json()
  } catch {
    return new Response('invalid JSON', { status: 400 })
  }

  if (!body.coreid || typeof body.data !== 'string') {
    return new Response('missing coreid or data', { status: 400 })
  }

  let rows: Bucket[]
  try {
    rows = parsePayload(body.coreid, body.data)
  } catch (e) {
    return new Response(e instanceof Error ? e.message : 'bad payload', {
      status: 400,
    })
  }

  if (rows.length === 0) {
    return Response.json({ inserted: 0, duplicates: 0 })
  }

  // ignoreDuplicates => ON CONFLICT DO NOTHING, and .select() returns only the
  // rows actually inserted, so a replay is a no-op that reports duplicates
  // rather than erroring or double-counting.
  const { data, error } = await supabase
    .from('activity_buckets')
    .upsert(rows, {
      onConflict: 'device_id,bucket_start',
      ignoreDuplicates: true,
    })
    .select()

  if (error) {
    return new Response(JSON.stringify({ error: error.message }), {
      status: 500,
      headers: { 'content-type': 'application/json' },
    })
  }

  const inserted = data?.length ?? 0
  return Response.json({ inserted, duplicates: rows.length - inserted })
})
