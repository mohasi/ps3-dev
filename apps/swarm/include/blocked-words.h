#pragma once

// Words that keep a result out of the search list. One rule: a title is split into words on anything
// that is not a letter or a digit, and if any of those words is in this list the result is not shown.
// Whole words only, so "topics" survives "pics" and "sexton" survives "sex".
//
// The list leans towards blocking. A word that is usually a tag and occasionally innocent is in it,
// so some ordinary titles go with the pornography: Big Bang Theory, Wicked, Adult Swim, National
// Gallery and Virtual Private Server all lose results. That is the trade that was asked for.
//
// Only words that carry no signal at all are left out. "18" is one: it would take Ubuntu 18.04 and
// every other version number without blocking anything the rest of the list misses.

static const char *BLOCKED_WORDS[] = {
   // the plain words
   "porn", "porno", "pornos", "pornographic", "pornography", "porns", "pron", "xxx", "sex",
   "sexo", "sexe", "sexy", "sexual", "sexuality", "hardcore", "softcore", "erotic", "erotica",
   "erotik", "nude", "nudes", "nudity", "naked", "nsfw", "smut", "lewd", "obscene", "uncensored",
   "explicit", "adult", "adults", "nudist", "naturist", "raunchy", "filthy", "dirty",

   // pictures, which is what a search for pictures brings back
   "pic", "pics", "picset", "picsets", "photoset", "photosets", "siterip", "siterips",
   "imageset", "imagesets", "selfie", "selfies", "bikini", "lingerie",
   "topless", "bottomless", "upskirt", "downblouse", "creepshot", "voyeur",

   // acts
   "anal", "analsex", "blowjob", "blowjobs", "handjob", "handjobs", "footjob", "titjob", "rimjob",
   "creampie", "creampies", "cumshot", "cumshots", "cumming", "cum", "facial", "facials", "bang",
   "banged", "banging", "penetration", "penetrated", "riding", "sucking", "sucks", "swallow",
   "swallowing", "screwing", "humping", "ejaculation", "seduction", "seduces", "seduced",
   "deepthroat", "throatfuck", "gangbang", "gangbangs", "bukkake", "threesome", "foursome", "orgy",
   "orgies", "masturbation", "masturbating", "masturbate", "fingering", "squirting", "squirt",
   "fisting", "pegging", "cowgirl", "doggystyle", "buttfuck", "fuck", "fucks", "fucked", "fucking",
   "fucker", "cuckold", "cuckolding",

   // body words used as tags
   "dildo", "dildos", "vibrator", "buttplug", "strapon", "cock", "cocks", "dick", "dicks", "pussy",
   "pussies", "vagina", "clit", "boob", "boobs", "boobed", "breast", "breasts", "breasted",
   "tits", "titty", "titties", "nipple", "nipples",
   "tit", "titted", "titfuck", "bigtit", "bigtits", "busty", "topheavy",
   "booty", "butt", "butts", "ass", "asses", "arse", "whore", "whores", "slut", "sluts", "slutty",
   "horny", "kinky", "kink", "lust", "lusty", "hentai", "ecchi", "yaoi", "yuri", "bitch", "bitches",
   "blonde", "blondes", "blondie", "brunette", "brunettes", "redhead", "redheads", "ginger",
   "latina", "latinas", "ebony", "asians", "babe", "chick", "chicks", "girlfriend", "gf", "wife",
   "wives", "housewife", "hotwife", "schoolgirl", "cheerleader", "nurse", "secretary", "maid",
   "curvy", "voluptuous", "seductive", "sensual", "intimate", "wet", "juicy", "tease", "teasing",
   "lesbian", "lesbians", "gay", "bisexual", "azz", "panties", "thong", "sexiness",
   "masturbator", "ejaculating", "fuckfest", "mommy", "stepmommy", "stepfam", "gravure",

   // how a site sorts them
   "milf", "milfs", "gilf", "cougar", "stepmom", "stepsis", "stepsister", "stepbro", "stepbrother",
   "stepdad", "stepdaughter", "stepson", "incest", "bdsm", "bondage", "fetish", "femdom",
   "submissive", "domination", "taboo", "candid", "gallery", "galleries", "megapack", "doujin",
   "doujinshi", "lolicon", "shotacon", "futanari", "shemale",
   "transsexual", "tranny", "ladyboy", "camgirl", "camgirls", "camwhore", "camshow", "camrip",
   "stripper", "striptease", "escort", "escorts", "hookup", "swingers", "sextape", "sextapes",
   "beastiality", "beastality", "bestiality", "zoophilia", "fappening", "onlyfans", "fansly",
   "manyvids", "thothub", "thots",

   // jav and its labels
   "jav", "javhd", "javbus", "javdb", "carib", "caribbeancom", "heyzo", "fc2ppv", "tokyohot",
   "1pondo", "10musume", "pacopacomama", "gachinco", "nyoshin", "uncen",

   // studios, as one word because that is how a release names them
   "brazzers", "brazzersexxtra", "bangbros", "naughtyamerica", "realitykings", "digitalplayground",
   "evilangel", "blacked", "blackedraw", "tushy", "tushyraw", "vixen", "slayed", "adulttime",
   "wicked", "deeper", "vivid", "private", "playtime", "playmate", "playmates", "centerfold",
   "teamskeet", "mofos", "twistys", "babes", "nubiles", "metart", "sexart", "wowgirls",
   "playboy", "penthouse", "hustler", "legalporno", "analvids", "dorcel", "woodmancastingx",
   "pierrewoodman", "bellesa", "girlsway", "sweetheartvideo", "hegreart", "hegre", "mypervyfamily",
   "pervymom", "familystrokes", "sislovesme", "bangbus", "mylf", "milfty", "exxxtrasmall",
   "letsdoeit", "pornmegaload", "onlygirls", "julesjordan", "elegantangel", "spizoo",
   "gotmepregnant", "puretaboo", "hardx", "darkx", "passionhd", "tiny4k", "cherrypimps", "nympho",
   "throated", "swallowed", "deeplush", "bratty", "wetandpuffy", "sexvent", "xfans", "skeet",
   "stepfampov", "japornxxx", "pornbull", "lifeselector", "inkasex", "dogfart", "dfxtra",
   "wanilianna", "nippon", "vixenmedia", "blowpass", "fakehub", "fakeagent", "publicagent",

   // tube sites, which end up in the name of a rip
   "pornhub", "phub", "xvideos", "xhamster", "xnxx", "redtube", "youporn", "tube8", "spankbang",
   "hqporner", "eporner", "txxx", "beeg", "fapello", "chaturbate", "camsoda", "stripchat",
   "bongacams", "myfreecams", "livejasmin", "streamate", "cam4", "motherless", "efukt"
};

#define BLOCKED_WORD_COUNT ((int)(sizeof BLOCKED_WORDS / sizeof BLOCKED_WORDS[0]))
