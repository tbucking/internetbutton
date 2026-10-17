// ingest-activity — receives a Particle webhook for the `activity` event and
// parses the compact CSV that firmware/activity-log publishes.
//
// WHY CSV AND NOT JSON. The cloud caps a single event payload at 1024 bytes, and
// JSON would spend much of that on punctuation and repeated key names. The
// firmware therefore sends:
//
//     v1,<startMin>,<n>,<count>,<sum>,<max>,<dz>;<count>,<sum>,<max>,<dz>;...
//
// This function is the single place that format is understood; everything
// downstream (Postgres, dashboards) sees ordinary rows.
//
// IDEMPOTENCY. The firmware retries any bucket it could not confirm, so the same
// bucket WILL arrive more than once. The table has
// `unique (device_id, bucket_start)` and this inserts with
// `ignoreDuplicates`, so a replay is a no-op rather than a duplicate row or an
// error. A closed bucket is immutable, so ignoring a duplicate loses nothing.
import { createClient } from "npm:@supabase/supabase-js@2";
const VERSION = 1;
const MAX_BUCKETS = 50; // matches BUCKETS_PER_PUB in the firmware
function json(body, status = 200) {
  return new Response(JSON.stringify(body), {
    status,
    headers: {
      "content-type": "application/json"
    }
  });
}
/**
 * Parse `v1,<startMin>,<n>,<count>,<sum>,<max>,<dz>;...` into rows.
 * Returns an error string instead of throwing so the reason is reportable.
 */ function parseCsv(raw, deviceId) {
  const text = raw.trim();
  if (!text.startsWith("v")) return {
    rows: [],
    error: "payload does not start with a version tag"
  };
  // header is exactly three fields; everything after the third comma is groups
  const first = text.indexOf(",");
  const second = text.indexOf(",", first + 1);
  const third = text.indexOf(",", second + 1);
  if (first < 0 || second < 0 || third < 0) return {
    rows: [],
    error: "truncated header"
  };
  const version = Number(text.slice(1, first));
  if (version !== VERSION) return {
    rows: [],
    error: `unsupported version ${version}`
  };
  const startMin = Number(text.slice(first + 1, second));
  const declared = Number(text.slice(second + 1, third));
  if (!Number.isFinite(startMin) || !Number.isFinite(declared)) {
    return {
      rows: [],
      error: "non-numeric header"
    };
  }
  const body = text.slice(third + 1);
  const groups = body.split(";").map((g)=>g.trim()).filter((g)=>g.length > 0);
  if (groups.length > MAX_BUCKETS) {
    return {
      rows: [],
      error: `too many buckets: ${groups.length} > ${MAX_BUCKETS}`
    };
  }
  if (declared !== groups.length) {
    // Not fatal: a truncated tail is possible if the payload was cut. Parse what
    // is actually there and let the caller see the discrepancy.
    console.warn(`declared ${declared} buckets, parsed ${groups.length}`);
  }
  const rows = [];
  for(let i = 0; i < groups.length; i++){
    const f = groups[i].split(",").map((v)=>Number(v.trim()));
    if (f.length !== 4 || f.some((v)=>!Number.isFinite(v) || v < 0)) {
      console.warn(`skipping malformed bucket ${i}: ${groups[i]}`);
      continue;
    }
    const [count, sum, max, dz] = f;
    rows.push({
      device_id: deviceId,
      // the firmware sends unix MINUTES; this is the bucket's start, in UTC
      bucket_start: new Date((startMin + i) * 60_000).toISOString(),
      impulses: count,
      impulse_sum: sum,
      // 0 is ambiguous rather than meaningful for these two, so store NULL
      max_delta: max === 0 ? null : max,
      max_dz: dz === 0 ? null : dz,
      batch: i
    });
  }
  return {
    rows
  };
}
Deno.serve(async (req)=>{
  if (req.method === "GET") {
    return json({
      ok: true,
      version: VERSION,
      accepts: "POST Particle webhook"
    });
  }
  if (req.method !== "POST") {
    return json({
      error: "method not allowed"
    }, 405);
  }
  let body;
  try {
    body = await req.json();
  } catch  {
    return json({
      error: "body is not JSON"
    }, 400);
  }
  // A Particle webhook wraps the event as { event, data, coreid, published_at }.
  // Accept a bare string too, so the endpoint can be tested with curl.
  let raw;
  let deviceId;
  if (typeof body === "string") {
    raw = body;
    deviceId = "manual";
  } else if (typeof body.data === "string") {
    raw = body.data;
    deviceId = typeof body.coreid === "string" && body.coreid.length > 0 ? body.coreid : "unknown";
  } else if (typeof body.payload === "string") {
    // Particle's webhook JSON template can nest it as `payload`
    raw = body.payload;
    deviceId = typeof body.coreid === "string" && body.coreid.length > 0 ? body.coreid : "unknown";
  } else {
    return json({
      error: "no data field in body"
    }, 400);
  }
  const { rows, error } = parseCsv(raw, deviceId);
  if (error) return json({
    error
  }, 400);
  if (rows.length === 0) return json({
    inserted: 0,
    note: "no parsable buckets"
  });
  const supabase = createClient(Deno.env.get("SUPABASE_URL"), // service role: bypasses RLS. Safe here because this function is the only
  // writer and the gateway has already required a JWT.
  Deno.env.get("SUPABASE_SERVICE_ROLE_KEY"), {
    auth: {
      persistSession: false
    }
  });
  const { data, error: dbError } = await supabase.from("activity_buckets").upsert(rows, {
    onConflict: "device_id,bucket_start",
    ignoreDuplicates: true
  }).select("bucket_start");
  if (dbError) {
    console.error("insert failed", dbError.message);
    return json({
      error: "database insert failed",
      detail: dbError.message
    }, 500);
  }
  const inserted = data?.length ?? 0;
  return json({
    ok: true,
    device_id: deviceId,
    parsed: rows.length,
    inserted,
    duplicates: rows.length - inserted
  });
});
