import type { TickDeltaMsg, VaRReportMsg, MarketAnchorsMsg } from '@/types/simulation';
import { MSG_HEADER_SIZE, MsgType, TICK_DELTA_SIZE, VAR_REPORT_SIZE, MARKET_ANCHORS_SIZE } from './schema';

export function parseMessageType(buf: ArrayBuffer): number {
  if (buf.byteLength < MSG_HEADER_SIZE) return -1;
  const view = new DataView(buf);
  return view.getUint8(0);
}

export function decodeTickDelta(buf: ArrayBuffer, offset = MSG_HEADER_SIZE): TickDeltaMsg | null {
  if (buf.byteLength < offset + TICK_DELTA_SIZE) return null;
  const v = new DataView(buf, offset);
  return {
    nodeId:        v.getUint32(0, true),
    riskScore:     v.getFloat32(4, true),
    nav:           v.getFloat64(8, true),
    exposureTotal: v.getFloat64(16, true),
    deltaNAV:      v.getFloat64(24, true),
    deltaExposure: v.getFloat64(32, true),
    isDefaulted:   v.getUint8(40) === 1,
    hubId:         v.getUint8(41),
    cascadeDepth:  v.getUint8(42),
    tickSeq:       v.getUint32(44, true),
    computeNs:     v.getBigUint64(48, true),
  };
}

export function decodeVaRReport(buf: ArrayBuffer, offset = MSG_HEADER_SIZE): VaRReportMsg | null {
  if (buf.byteLength < offset + VAR_REPORT_SIZE) return null;
  const v = new DataView(buf, offset);
  return {
    targetNode: v.getUint32(0, true),
    pathsRun:   v.getUint32(4, true),
    var95:      v.getFloat64(8, true),
  };
}

export function decodeMarketAnchors(buf: ArrayBuffer, offset = MSG_HEADER_SIZE): MarketAnchorsMsg | null {
  if (buf.byteLength < offset + MARKET_ANCHORS_SIZE) return null;
  const v = new DataView(buf, offset);
  return {
    equities:   v.getFloat64(0, true),
    realEstate: v.getFloat64(8, true),
    crypto:     v.getFloat64(16, true),
    treasuries: v.getFloat64(24, true),
    corpBonds:  v.getFloat64(32, true),
  };
}
