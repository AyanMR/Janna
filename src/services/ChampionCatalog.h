#pragma once

#include <QList>

namespace Janna {

// Data Dragon's locale-independent champion keys paired with the Simplified
// Chinese display names.  Keeping this small catalog in the client means the
// OPGG view can still resolve names and portraits when the LCU is unavailable
// or is running in an English locale.
struct ChampionCatalogEntry {
    int id;
    const char *key;
    const char *name;
};

inline const QList<ChampionCatalogEntry> &championCatalog()
{
    static const QList<ChampionCatalogEntry> entries = {
        {266,"Aatrox","暗裔剑魔"},{103,"Ahri","九尾妖狐"},{84,"Akali","离群之刺"},{166,"Akshan","影哨"},
        {12,"Alistar","牛头酋长"},{799,"Ambessa","铁血狼母"},{32,"Amumu","殇之木乃伊"},{34,"Anivia","冰晶凤凰"},
        {1,"Annie","黑暗之女"},{523,"Aphelios","残月之肃"},{22,"Ashe","寒冰射手"},{136,"AurelionSol","铸星龙王"},
        {893,"Aurora","双界灵兔"},{268,"Azir","沙漠皇帝"},{432,"Bard","星界游神"},{200,"Belveth","虚空女皇"},
        {53,"Blitzcrank","蒸汽机器人"},{63,"Brand","复仇焰魂"},{201,"Braum","弗雷尔卓德之心"},{233,"Briar","狂厄蔷薇"},
        {51,"Caitlyn","皮城女警"},{164,"Camille","青钢影"},{69,"Cassiopeia","魔蛇之拥"},{31,"Chogath","虚空恐惧"},
        {42,"Corki","英勇投弹手"},{122,"Darius","诺克萨斯之手"},{131,"Diana","皎月女神"},{119,"Draven","荣耀行刑官"},
        {36,"DrMundo","祖安狂人"},{245,"Ekko","时间刺客"},{60,"Elise","蜘蛛女皇"},{28,"Evelynn","痛苦之拥"},
        {81,"Ezreal","探险家"},{9,"Fiddlesticks","远古恐惧"},{114,"Fiora","无双剑姬"},{105,"Fizz","潮汐海灵"},
        {3,"Galio","正义巨像"},{41,"Gangplank","海洋之灾"},{86,"Garen","德玛西亚之力"},{150,"Gnar","迷失之牙"},
        {79,"Gragas","酒桶"},{104,"Graves","法外狂徒"},{887,"Gwen","灵罗娃娃"},{120,"Hecarim","战争之影"},
        {74,"Heimerdinger","大发明家"},{910,"Hwei","异画师"},{420,"Illaoi","海兽祭司"},{39,"Irelia","刀锋舞者"},
        {427,"Ivern","翠神"},{40,"Janna","风暴之怒"},{59,"JarvanIV","德玛西亚皇子"},{24,"Jax","武器大师"},
        {126,"Jayce","未来守护者"},{202,"Jhin","戏命师"},{222,"Jinx","暴走萝莉"},{145,"Kaisa","虚空之女"},
        {429,"Kalista","复仇之矛"},{43,"Karma","天启者"},{30,"Karthus","死亡颂唱者"},{38,"Kassadin","虚空行者"},
        {55,"Katarina","不祥之刃"},{10,"Kayle","正义天使"},{141,"Kayn","影流之镰"},{85,"Kennen","狂暴之心"},
        {121,"Khazix","虚空掠夺者"},{203,"Kindred","永猎双子"},{240,"Kled","暴怒骑士"},{96,"KogMaw","深渊巨口"},
        {897,"KSante","纳祖芒荣耀"},{7,"Leblanc","诡术妖姬"},{64,"LeeSin","盲僧"},{89,"Leona","曙光女神"},
        {876,"Lillia","含羞蓓蕾"},{127,"Lissandra","冰霜女巫"},{805,"Locke","灰烬驱魔人"},{236,"Lucian","圣枪游侠"},
        {117,"Lulu","仙灵女巫"},{99,"Lux","光辉女郎"},{54,"Malphite","熔岩巨兽"},{90,"Malzahar","虚空先知"},
        {57,"Maokai","扭曲树精"},{11,"MasterYi","无极剑圣"},{800,"Mel","流光镜影"},{902,"Milio","明烛"},
        {21,"MissFortune","赏金猎人"},{62,"MonkeyKing","齐天大圣"},{82,"Mordekaiser","铁铠冥魂"},{25,"Morgana","堕落天使"},
        {950,"Naafiri","百裂冥犬"},{267,"Nami","唤潮鲛姬"},{75,"Nasus","沙漠死神"},{111,"Nautilus","深海泰坦"},
        {518,"Neeko","万花通灵"},{76,"Nidalee","狂野女猎手"},{895,"Nilah","不羁之悦"},{56,"Nocturne","永恒梦魇"},
        {20,"Nunu","雪原双子"},{2,"Olaf","狂战士"},{61,"Orianna","发条魔灵"},{516,"Ornn","山隐之焰"},
        {80,"Pantheon","不屈之枪"},{78,"Poppy","圣锤之毅"},{555,"Pyke","血港鬼影"},{246,"Qiyana","元素女皇"},
        {133,"Quinn","德玛西亚之翼"},{497,"Rakan","幻翎"},{33,"Rammus","披甲龙龟"},{421,"RekSai","虚空遁地兽"},
        {526,"Rell","镕铁少女"},{888,"Renata","炼金男爵"},{58,"Renekton","荒漠屠夫"},{107,"Rengar","傲之追猎者"},
        {92,"Riven","放逐之刃"},{68,"Rumble","机械公敌"},{13,"Ryze","符文法师"},{360,"Samira","沙漠玫瑰"},
        {113,"Sejuani","北地之怒"},{235,"Senna","涤魂圣枪"},{147,"Seraphine","星籁歌姬"},{875,"Sett","腕豪"},
        {35,"Shaco","恶魔小丑"},{98,"Shen","暮光之眼"},{102,"Shyvana","龙血武姬"},{27,"Singed","炼金术士"},
        {14,"Sion","亡灵战神"},{15,"Sivir","战争女神"},{72,"Skarner","上古领主"},{901,"Smolder","炽炎雏龙"},
        {37,"Sona","琴瑟仙女"},{16,"Soraka","众星之子"},{50,"Swain","诺克萨斯统领"},{517,"Sylas","解脱者"},
        {134,"Syndra","暗黑元首"},{223,"TahmKench","河流之王"},{163,"Taliyah","岩雀"},{91,"Talon","刀锋之影"},
        {44,"Taric","瓦洛兰之盾"},{17,"Teemo","迅捷斥候"},{412,"Thresh","魂锁典狱长"},{18,"Tristana","麦林炮手"},
        {48,"Trundle","巨魔之王"},{23,"Tryndamere","蛮族之王"},{4,"TwistedFate","卡牌大师"},{29,"Twitch","瘟疫之源"},
        {77,"Udyr","兽灵行者"},{6,"Urgot","无畏战车"},{110,"Varus","惩戒之箭"},{67,"Vayne","暗夜猎手"},
        {45,"Veigar","邪恶小法师"},{161,"Velkoz","虚空之眼"},{711,"Vex","愁云使者"},{254,"Vi","皮城执法官"},
        {234,"Viego","破败之王"},{112,"Viktor","奥术先驱"},{8,"Vladimir","猩红收割者"},{106,"Volibear","不灭狂雷"},
        {19,"Warwick","祖安怒兽"},{498,"Xayah","逆羽"},{101,"Xerath","远古巫灵"},{5,"XinZhao","德邦总管"},
        {157,"Yasuo","疾风剑豪"},{777,"Yone","封魔剑魂"},{83,"Yorick","牧魂人"},{804,"Yunara","不破之誓"},
        {350,"Yuumi","魔法猫咪"},{904,"Zaahen","不落魔锋"},{154,"Zac","生化魔人"},{238,"Zed","影流之主"},
        {221,"Zeri","祖安花火"},{115,"Ziggs","爆破鬼才"},{26,"Zilean","时光守护者"},{142,"Zoe","暮光星灵"},
        {143,"Zyra","荆棘之兴"}
    };
    return entries;
}

} // namespace Janna
