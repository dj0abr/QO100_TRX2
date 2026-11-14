import entities from './dxcc-data.js';

const prefixes = new Map(), exactCalls = new Map();
for (const [entity, code, prefixList, callList] of entities) {
  const location = {entity, code};
  for (const prefix of prefixList.split(' ')) if (prefix) prefixes.set(prefix, location);
  for (const call of callList.split(' ')) if (call) exactCalls.set(call, location);
}

export function dxccForCall(source) {
  const call = source.replace(/^<|>$/g, '').toUpperCase();
  if (!/^[A-Z0-9]+(?:\/[A-Z0-9]+)*$/.test(call) || /\/(?:MM|AM)$/.test(call)) return null;
  if (exactCalls.has(call)) return exactCalls.get(call);

  const parts = call.split('/');
  const base = parts.find(part => /^[A-Z0-9]*\d[A-Z]+$/.test(part));
  const area = parts.find(part => part !== base && !/^(?:P|M|R|QRP|QRPP|A|\d)$/.test(part));
  // Try an explicit slash prefix first, then the operating prefix or base call.
  for (const candidate of [call, area, base]) {
    if (!candidate) continue;
    const slash = candidate.indexOf('/');
    const minLength = slash < 0 ? 1 : slash + 1;
    for (let end = candidate.length; end >= minLength; end--) {
      const location = prefixes.get(candidate.slice(0, end));
      if (location) return location;
    }
  }
  return null;
}

// Order of the graphics in dxcc-flags.png. See dxcc-LICENSE.txt.
const flagCodes = [
  'AD AE AF AG AI AL AM AO AQ AR AS AT AU AW AX AZ',
  'BA BB BD BE BF BG BH BI BJ BL BM BN BO BQ BR BS',
  'BT BV BW BY BZ CA CC CD CF CG CH CI CK CL CM CN',
  'CO CR CU CV CW CX CY CZ DE DJ DK DM DO DZ EC EE',
  'EG EH ER ES ET FI FJ FK FM FO FR GA GB GD GE GF',
  'GG GH GI GL GM GN GP GQ GR GT GU GW GY HK HM HN',
  'HR HT HU ID IE IL IM IN IO IQ IR IS IT JE JM JO',
  'JP KE KG KH KI KM KN KP KR KW KY KZ LA LB LC LI',
  'LK LR LS LT LU LV LY MA MC MD ME MF MG MH MK ML',
  'MM MN MO MP MQ MR MS MT MU MV MW MX MY MZ NA NC',
  'NE NG NI NL NO NP NR NU NZ OM PA PE PF PG PH PK',
  'PL PM PN PR PS PT PW PY QA RE RO RS RU RW SA SB',
  'SC SD SE SG SH SI SJ SK SL SM SN SO SR SS ST SV',
  'SX SY SZ TC TD TF TG TH TJ TK TL TM TN TO TR TT',
  'TV TW TZ UA UG UM UN US UY UZ VA VC VE VG VI VN',
  'VU WF WS XK YE YT ZA ZM ZW',
].join(' ').split(' ');
const flagPositions = new Map(flagCodes.map((code, i) => [code, i]));

export function countryFlagPosition(code) {
  const index = flagPositions.get(code);
  return index === undefined ? '' : `${-18 * (index % 16)}px ${-18 * Math.floor(index / 16)}px`;
}
