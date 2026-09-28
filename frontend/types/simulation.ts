export type SimPhase =
  | 'pre_shock'
  | 'shock_triggered'
  | 'cascade_running'
  | 'cascade_complete'
  | 'paused';

export type ShockType = 'custom' | 'lehman2008' | 'covid2020' | 'rate_hike' | 'crypto_crash';

export interface ShockConfig {
  targetNodeId: number;      // 0xFFFFFFFF = market-wide
  shockType: ShockType;
  equitiesDelta: number;     // e.g. -0.40 = -40%
  realEstateDelta: number;
  cryptoDelta: number;
  treasuriesDelta: number;
  corpBondsDelta: number;
}

export const SHOCK_PRESETS: Record<Exclude<ShockType, 'custom'>, Omit<ShockConfig, 'targetNodeId' | 'shockType'>> = {
  lehman2008: { equitiesDelta: -0.40, realEstateDelta: -0.25, cryptoDelta: 0, treasuriesDelta: 0.05, corpBondsDelta: -0.15 },
  covid2020:  { equitiesDelta: -0.35, realEstateDelta: -0.10, cryptoDelta: -0.50, treasuriesDelta: 0.08, corpBondsDelta: -0.05 },
  rate_hike:  { equitiesDelta: -0.08, realEstateDelta: -0.12, cryptoDelta: -0.20, treasuriesDelta: -0.20, corpBondsDelta: -0.12 },
  crypto_crash: { equitiesDelta: -0.05, realEstateDelta: 0, cryptoDelta: -0.80, treasuriesDelta: 0.02, corpBondsDelta: 0 },
};

export interface TickDeltaMsg {
  nodeId: number;
  riskScore: number;
  nav: number;
  exposureTotal: number;
  deltaNAV: number;
  deltaExposure: number;
  isDefaulted: boolean;
  hubId: number;
  cascadeDepth: number;
  tickSeq: number;
  computeNs: bigint;
}

export interface VaRReportMsg {
  targetNode: number;
  pathsRun: number;
  var95: number;
}

export interface MarketAnchorsMsg {
  equities: number;
  realEstate: number;
  crypto: number;
  treasuries: number;
  corpBonds: number;
}

export interface SimEvent {
  id: string;
  type: 'shock_sent' | 'node_default' | 'var_report' | 'cascade_start' | 'cascade_end' | 'connected' | 'disconnected';
  timestamp: number;
  label: string;
  nodeId?: number;
  shockType?: ShockType;
  var95?: number;
  cascadeDepth?: number;
}
