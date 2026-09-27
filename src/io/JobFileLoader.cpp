#include "io/JobFileLoader.h"
#include "imaging/JobSpatialIndex.h"

#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QStringDecoder>
#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace {

constexpr qint64 kMaximumJobPoints = 1'000'000;
constexpr int kMaximumJsonDepth = 64;
constexpr qsizetype kReadBufferBytes = 64 * 1024;

class JsonReader {
public:
    JsonReader(QFile& file, CancellationTokenPtr token, JobFileLoader::Progress progress)
        : file_(file), token_(std::move(token)), progress_(std::move(progress)), total_(file.size()) {}

    [[nodiscard]] qint64 position() const noexcept { return position_; }
    [[nodiscard]] bool cancelled() const noexcept { return token_ && token_->isCancelled(); }

    bool atEnd() {
        skipWhitespace();
        return !ensure();
    }

    bool consume(char expected) {
        skipWhitespace();
        if (!ensure() || buffer_[index_] != expected) return false;
        take();
        return true;
    }

    bool expect(char expected, QString* error, const QString& description) {
        if (consume(expected)) return true;
        fail(error, QStringLiteral("期望%1").arg(description));
        return false;
    }

    void skipWhitespace() {
        while (ensure()) {
            const char c = buffer_[index_];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            take();
        }
    }

    bool parseString(QString* value, QString* error) {
        skipWhitespace();
        if (!ensure() || buffer_[index_] != '"') {
            fail(error, QStringLiteral("期望字符串"));
            return false;
        }
        take();
        QByteArray utf8;
        QString result;
        const auto flushUtf8 = [&]() -> bool {
            if (utf8.isEmpty()) return true;
            QStringDecoder decoder(QStringDecoder::Utf8);
            const QString decoded = decoder.decode(utf8);
            if (decoder.hasError()) {
                fail(error, QStringLiteral("字符串包含无效 UTF-8"));
                return false;
            }
            result += decoded;
            utf8.clear();
            return true;
        };
        while (ensure()) {
            const unsigned char c = static_cast<unsigned char>(take());
            if (c == '"') {
                if (!flushUtf8()) return false;
                *value = std::move(result);
                return true;
            }
            if (c < 0x20) {
                fail(error, QStringLiteral("字符串包含未转义控制字符"));
                return false;
            }
            if (c != '\\') {
                utf8.append(static_cast<char>(c));
                continue;
            }
            if (!flushUtf8() || !ensure()) {
                fail(error, QStringLiteral("字符串转义不完整"));
                return false;
            }
            const char escaped = take();
            switch (escaped) {
            case '"': result += QLatin1Char('"'); break;
            case '\\': result += QLatin1Char('\\'); break;
            case '/': result += QLatin1Char('/'); break;
            case 'b': result += QLatin1Char('\b'); break;
            case 'f': result += QLatin1Char('\f'); break;
            case 'n': result += QLatin1Char('\n'); break;
            case 'r': result += QLatin1Char('\r'); break;
            case 't': result += QLatin1Char('\t'); break;
            case 'u': {
                ushort first = 0;
                if (!parseHex4(&first, error)) return false;
                if (QChar::isHighSurrogate(first)) {
                    if (!ensure() || take() != '\\' || !ensure() || take() != 'u') {
                        fail(error, QStringLiteral("高代理项后缺少低代理项"));
                        return false;
                    }
                    ushort second = 0;
                    if (!parseHex4(&second, error) || !QChar::isLowSurrogate(second)) {
                        fail(error, QStringLiteral("无效低代理项"));
                        return false;
                    }
                    const char32_t ucs4 = QChar::surrogateToUcs4(first, second);
                    const char32_t text[] = {ucs4, 0};
                    result += QString::fromUcs4(text);
                } else if (QChar::isLowSurrogate(first)) {
                    fail(error, QStringLiteral("孤立低代理项"));
                    return false;
                } else {
                    result += QChar(first);
                }
                break;
            }
            default:
                fail(error, QStringLiteral("无效字符串转义"));
                return false;
            }
        }
        fail(error, QStringLiteral("字符串未结束"));
        return false;
    }

    bool parseDouble(double* value, QString* error) {
        QByteArray number;
        if (!parseNumberToken(&number, error)) return false;
        bool ok = false;
        const double parsed = QLocale::c().toDouble(QString::fromLatin1(number), &ok);
        if (!ok || !std::isfinite(parsed)) {
            fail(error, QStringLiteral("数字无效或超出范围"));
            return false;
        }
        *value = parsed;
        return true;
    }

    bool parseInteger(qint64* value, QString* error) {
        QByteArray number;
        if (!parseNumberToken(&number, error)) return false;
        if (number.contains('.') || number.contains('e') || number.contains('E')) {
            fail(error, QStringLiteral("place.id 必须是整数"));
            return false;
        }
        bool ok = false;
        const qint64 parsed = number.toLongLong(&ok);
        if (!ok) {
            fail(error, QStringLiteral("整数超出范围"));
            return false;
        }
        *value = parsed;
        return true;
    }

    bool skipValue(QString* error, int depth = 0) {
        if (depth > kMaximumJsonDepth) {
            fail(error, QStringLiteral("JSON 嵌套层级过深"));
            return false;
        }
        skipWhitespace();
        if (!ensure()) {
            fail(error, QStringLiteral("JSON 值不完整"));
            return false;
        }
        const char c = buffer_[index_];
        if (c == '"') { QString ignored; return parseString(&ignored, error); }
        if (c == '{') {
            take(); skipWhitespace();
            if (consume('}')) return true;
            while (true) {
                QString key;
                if (!parseString(&key, error) || !expect(':', error, QStringLiteral("冒号")) ||
                    !skipValue(error, depth + 1)) return false;
                if (consume('}')) return true;
                if (!expect(',', error, QStringLiteral("逗号"))) return false;
            }
        }
        if (c == '[') {
            take(); skipWhitespace();
            if (consume(']')) return true;
            while (true) {
                if (!skipValue(error, depth + 1)) return false;
                if (consume(']')) return true;
                if (!expect(',', error, QStringLiteral("逗号"))) return false;
            }
        }
        if (c == '-' || (c >= '0' && c <= '9')) { QByteArray ignored; return parseNumberToken(&ignored, error); }
        if (c == 't') return expectLiteral("true", error);
        if (c == 'f') return expectLiteral("false", error);
        if (c == 'n') return expectLiteral("null", error);
        fail(error, QStringLiteral("无法识别的 JSON 值"));
        return false;
    }

    void fail(QString* error, const QString& message) const {
        if (error && error->isEmpty()) *error = QStringLiteral("字节 %1：%2").arg(position_).arg(message);
    }

private:
    bool ensure() {
        if (index_ < buffer_.size()) return true;
        if (eof_ || cancelled()) return false;
        buffer_ = file_.read(kReadBufferBytes);
        index_ = 0;
        if (buffer_.isEmpty()) { eof_ = true; return false; }
        if (progress_ && (position_ == 0 || position_ - lastProgress_ >= 1024 * 1024)) {
            lastProgress_ = position_;
            progress_({QStringLiteral("读取 Job 文件"), position_, std::max<qint64>(1, total_)});
        }
        return true;
    }

    char take() {
        if (!ensure()) return '\0';
        ++position_;
        return buffer_[index_++];
    }

    bool expectLiteral(const char* literal, QString* error) {
        for (const char* p = literal; *p; ++p) {
            if (!ensure() || take() != *p) {
                fail(error, QStringLiteral("JSON 字面量无效"));
                return false;
            }
        }
        return true;
    }

    bool parseHex4(ushort* value, QString* error) {
        ushort result = 0;
        for (int i = 0; i < 4; ++i) {
            if (!ensure()) { fail(error, QStringLiteral("Unicode 转义不完整")); return false; }
            const char c = take();
            int digit = -1;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            if (digit < 0) { fail(error, QStringLiteral("Unicode 转义包含非十六进制字符")); return false; }
            result = static_cast<ushort>((result << 4) | digit);
        }
        *value = result;
        return true;
    }

    bool parseNumberToken(QByteArray* number, QString* error) {
        skipWhitespace(); number->clear();
        const auto append = [&] { number->append(take()); };
        if (ensure() && buffer_[index_] == '-') append();
        if (!ensure()) { fail(error, QStringLiteral("数字不完整")); return false; }
        if (buffer_[index_] == '0') append();
        else if (buffer_[index_] >= '1' && buffer_[index_] <= '9') {
            do { append(); } while (ensure() && buffer_[index_] >= '0' && buffer_[index_] <= '9');
        } else { fail(error, QStringLiteral("无效数字")); return false; }
        if (ensure() && buffer_[index_] == '.') {
            append();
            if (!ensure() || buffer_[index_] < '0' || buffer_[index_] > '9') { fail(error, QStringLiteral("小数部分不完整")); return false; }
            do { append(); } while (ensure() && buffer_[index_] >= '0' && buffer_[index_] <= '9');
        }
        if (ensure() && (buffer_[index_] == 'e' || buffer_[index_] == 'E')) {
            append();
            if (ensure() && (buffer_[index_] == '+' || buffer_[index_] == '-')) append();
            if (!ensure() || buffer_[index_] < '0' || buffer_[index_] > '9') { fail(error, QStringLiteral("指数部分不完整")); return false; }
            do { append(); } while (ensure() && buffer_[index_] >= '0' && buffer_[index_] <= '9');
        }
        return true;
    }

    QFile& file_;
    CancellationTokenPtr token_;
    JobFileLoader::Progress progress_;
    QByteArray buffer_;
    qsizetype index_ = 0;
    qint64 position_ = 0;
    qint64 total_ = 0;
    qint64 lastProgress_ = -1024 * 1024;
    bool eof_ = false;
};

bool parseModel(JsonReader& reader, JobModel* model, QString* error) {
    if (!reader.expect('{', error, QStringLiteral("模型对象"))) return false;
    bool hasLabel = false, hasPath = false;
    if (reader.consume('}')) { reader.fail(error, QStringLiteral("模型缺少 label/path")); return false; }
    while (true) {
        QString key;
        if (!reader.parseString(&key, error) || !reader.expect(':', error, QStringLiteral("冒号"))) return false;
        if (key == QStringLiteral("label")) { if (!reader.parseString(&model->label, error)) return false; hasLabel = true; }
        else if (key == QStringLiteral("path")) { if (!reader.parseString(&model->path, error)) return false; hasPath = true; }
        else if (!reader.skipValue(error)) return false;
        if (reader.consume('}')) break;
        if (!reader.expect(',', error, QStringLiteral("逗号"))) return false;
    }
    if (!hasLabel || !hasPath) { reader.fail(error, QStringLiteral("模型缺少字符串 label/path")); return false; }
    return true;
}

enum class ParsedCommandType { Move, Place };
struct ParsedCommand { ParsedCommandType type; JobCoordinate position; qint64 id = -1; };

bool parseBy(JsonReader& reader, JobCoordinate* position, QString* error) {
    if (!reader.expect('[', error, QStringLiteral("坐标数组"))) return false;
    double values[3]{};
    for (int i = 0; i < 3; ++i) {
        if (!reader.parseDouble(&values[i], error)) return false;
        if (i < 2 && !reader.expect(',', error, QStringLiteral("坐标逗号"))) return false;
    }
    if (!reader.expect(']', error, QStringLiteral("坐标数组结束符"))) return false;
    for (double value : values) {
        if (value < -std::numeric_limits<float>::max() || value > std::numeric_limits<float>::max()) {
            reader.fail(error, QStringLiteral("坐标超出渲染范围")); return false;
        }
    }
    *position = {values[0], values[1], values[2]};
    return true;
}

bool parseCommand(JsonReader& reader, ParsedCommand* parsed, QString* error) {
    if (!reader.expect('{', error, QStringLiteral("命令对象"))) return false;
    QString command;
    JobCoordinate by, where;
    qint64 id = -1;
    bool hasCommand = false, hasBy = false, hasWhere = false, hasId = false;
    if (reader.consume('}')) { reader.fail(error, QStringLiteral("空命令对象")); return false; }
    while (true) {
        QString key;
        if (!reader.parseString(&key, error) || !reader.expect(':', error, QStringLiteral("冒号"))) return false;
        if (key == QStringLiteral("command")) { if (!reader.parseString(&command, error)) return false; hasCommand = true; }
        else if (key == QStringLiteral("by")) { if (!parseBy(reader, &by, error)) return false; hasBy = true; }
        else if (key == QStringLiteral("where")) { if (!parseBy(reader, &where, error)) return false; hasWhere = true; }
        else if (key == QStringLiteral("id")) { if (!reader.parseInteger(&id, error)) return false; hasId = true; }
        else if (!reader.skipValue(error)) return false;
        if (reader.consume('}')) break;
        if (!reader.expect(',', error, QStringLiteral("逗号"))) return false;
    }
    if (!hasCommand) { reader.fail(error, QStringLiteral("命令缺少 command 字段")); return false; }
    if (command == QStringLiteral("relmove")) {
        if (!hasBy || hasWhere || hasId) { reader.fail(error, QStringLiteral("relmove 必须仅包含 by 坐标")); return false; }
        parsed->type = ParsedCommandType::Move; parsed->position = by;
    } else if (command == QStringLiteral("absmove")) {
        if (hasBy == hasWhere || hasId) { reader.fail(error, QStringLiteral("absmove 必须仅包含 where 坐标")); return false; }
        parsed->type = ParsedCommandType::Move; parsed->position = hasWhere ? where : by;
    } else if (command == QStringLiteral("place")) {
        if (!hasId || hasBy || hasWhere) { reader.fail(error, QStringLiteral("place 必须仅包含整数 id")); return false; }
        parsed->type = ParsedCommandType::Place; parsed->id = id;
    } else {
        reader.fail(error, QStringLiteral("不支持的命令类型：%1").arg(command)); return false;
    }
    return true;
}

bool parseModels(JsonReader& reader, JobDocument& document, QString* error) {
    if (!reader.expect('[', error, QStringLiteral("models 数组"))) return false;
    if (reader.consume(']')) return true;
    while (true) {
        if (document.models.size() >= static_cast<size_t>(kMaximumJobPoints)) {
            reader.fail(error, QStringLiteral("模型数量超过 %1").arg(kMaximumJobPoints)); return false;
        }
        JobModel model;
        model.id = static_cast<qint64>(document.models.size());
        if (!parseModel(reader, &model, error)) return false;
        document.models.push_back(std::move(model));
        if (reader.consume(']')) return true;
        if (!reader.expect(',', error, QStringLiteral("模型逗号"))) return false;
    }
}

bool parseCommands(JsonReader& reader, JobDocument& document, QString* error) {
    if (!reader.expect('[', error, QStringLiteral("commands 数组"))) return false;
    if (reader.consume(']')) return true;
    qint64 commandIndex = 0;
    JobCoordinate pendingPosition;
    bool expectingMove = true;
    while (true) {
        ParsedCommand command;
        if (!parseCommand(reader, &command, error)) return false;
        if (expectingMove) {
            if (command.type != ParsedCommandType::Move) {
                reader.fail(error, QStringLiteral("命令 %1：期望 relmove 或 absmove").arg(commandIndex)); return false;
            }
            pendingPosition = command.position;
        } else {
            if (command.type != ParsedCommandType::Place) {
                reader.fail(error, QStringLiteral("命令 %1：期望 place").arg(commandIndex)); return false;
            }
            if (document.points.size() >= static_cast<size_t>(kMaximumJobPoints)) {
                reader.fail(error, QStringLiteral("点数量超过 %1").arg(kMaximumJobPoints)); return false;
            }
            document.points.push_back({command.id, pendingPosition, commandIndex,
                                       static_cast<qint64>(document.points.size())});
            document.bounds.include(pendingPosition);
        }
        expectingMove = !expectingMove;
        ++commandIndex;
        if (reader.consume(']')) {
            if (!expectingMove) { reader.fail(error, QStringLiteral("最后一个移动命令缺少对应 place")); return false; }
            return true;
        }
        if (!reader.expect(',', error, QStringLiteral("命令逗号"))) return false;
    }
}

} // namespace

JobDocumentPtr JobFileLoader::load(const QString& path, const CancellationTokenPtr& token,
                                   const Progress& progress, QString* error) {
    if (error) error->clear();
    QFileInfo info(path);
    const QString suffix = info.suffix().toLower();
    if (suffix != QStringLiteral("json") && suffix != QStringLiteral("aldl")) {
        if (error) *error = QStringLiteral("只支持 .json 和 .aldl Job 文件。");
        return {};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法打开 Job 文件：%1").arg(file.errorString());
        return {};
    }
    if (file.peek(3) == QByteArray("\xEF\xBB\xBF", 3)) file.seek(3);
    if (!token) {
        if (error) *error = QStringLiteral("Job 加载缺少取消令牌。");
        return {};
    }
    try {
        auto document = std::make_shared<JobDocument>();
        document->source = JobDocumentSource::ImportedFile;
        document->sourcePath = info.absoluteFilePath();
        JsonReader reader(file, token, progress);
        if (!reader.expect('{', error, QStringLiteral("根对象"))) return {};
        bool hasModels = false, hasCommands = false;
        if (reader.consume('}')) { if (error) *error = QStringLiteral("Job 根对象为空。"); return {}; }
        while (true) {
            if (token->isCancelled()) return {};
            QString key;
            if (!reader.parseString(&key, error) || !reader.expect(':', error, QStringLiteral("冒号"))) return {};
            if (key == QStringLiteral("models")) {
                if (hasModels) { reader.fail(error, QStringLiteral("models 字段重复")); return {}; }
                hasModels = true;
                if (!parseModels(reader, *document, error)) return {};
            } else if (key == QStringLiteral("commands")) {
                if (hasCommands) { reader.fail(error, QStringLiteral("commands 字段重复")); return {}; }
                hasCommands = true;
                if (!parseCommands(reader, *document, error)) return {};
            } else if (!reader.skipValue(error)) return {};
            if (reader.consume('}')) break;
            if (!reader.expect(',', error, QStringLiteral("根对象逗号"))) return {};
        }
        if (!reader.atEnd()) { reader.fail(error, QStringLiteral("根对象后存在多余内容")); return {}; }
        if (!hasModels || !hasCommands) {
            if (error) *error = QStringLiteral("Job 必须同时包含 models 和 commands 数组。");
            return {};
        }
        if (document->models.empty() || document->points.empty()) {
            if (error) *error = QStringLiteral("Job 不包含可预览的模型和点。");
            return {};
        }
        const JobCoordinate extent=document->bounds.size();
        if(extent.x>std::numeric_limits<float>::max()||extent.y>std::numeric_limits<float>::max()||extent.z>std::numeric_limits<float>::max()){
            if(error)*error=QStringLiteral("Job 坐标范围过大，无法安全转换为 GPU 相对坐标。");
            return {};
        }
        for (const JobPoint& point : document->points) {
            if (point.placeId < 0 ||
                point.placeId >= static_cast<qint64>(document->models.size())) {
                if (error) *error = QStringLiteral("命令 %1 的 place.id=%2 超出模型范围。")
                                        .arg(point.commandIndex).arg(point.placeId);
                return {};
            }
        }
        document->sceneBounds = document->bounds;
        if(progress)progress({QStringLiteral("建立 Job 空间索引"),0,static_cast<qint64>(document->points.size())});
        if (!JobSpatialIndex::prepare(document.get(),token)) {
            if (!token->isCancelled() && error) *error=QStringLiteral("无法建立 Job 空间索引。");
            return {};
        }
        if (progress) progress({QStringLiteral("校验并索引 Job 文件"), file.size(), std::max<qint64>(1, file.size())});
        return document;
    } catch (const std::bad_alloc&) {
        if (error) *error = QStringLiteral("Job 文件过大，内存不足。");
        return {};
    }
}
