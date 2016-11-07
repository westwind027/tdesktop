/*
This file is part of Telegram Desktop,
the official desktop version of Telegram messaging app, see https://telegram.org

Telegram Desktop is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

It is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

In addition, as a special exception, the copyright holders give permission
to link the code of portions of this program with the OpenSSL library.

Full license: https://github.com/telegramdesktop/tdesktop/blob/master/LICENSE
Copyright (c) 2014-2016 John Preston, https://desktop.telegram.org
*/
#include "codegen/style/generator.h"

#include <memory>
#include <functional>
#include <QtCore/QDir>
#include <QtCore/QSet>
#include <QtCore/QBuffer>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include "codegen/style/parsed_file.h"

using Module = codegen::style::structure::Module;
using Struct = codegen::style::structure::Struct;
using Variable = codegen::style::structure::Variable;
using Tag = codegen::style::structure::TypeTag;

namespace codegen {
namespace style {
namespace {

constexpr int kErrorBadIconSize     = 861;
constexpr int kErrorBadIconFormat   = 862;

// crc32 hash, taken somewhere from the internet

class Crc32Table {
public:
	Crc32Table() {
		quint32 poly = 0x04c11db7;
		for (auto i = 0; i != 256; ++i) {
			_data[i] = reflect(i, 8) << 24;
			for (auto j = 0; j != 8; ++j) {
				_data[i] = (_data[i] << 1) ^ (_data[i] & (1 << 31) ? poly : 0);
			}
			_data[i] = reflect(_data[i], 32);
		}
	}

	inline quint32 operator[](int index) const {
		return _data[index];
	}

private:
	quint32 reflect(quint32 val, char ch) {
		quint32 result = 0;
		for (int i = 1; i < (ch + 1); ++i) {
			if (val & 1) {
				result |= 1 << (ch - i);
			}
			val >>= 1;
		}
		return result;
	}

	quint32 _data[256];

};

qint32 hashCrc32(const void *data, int len) {
	static Crc32Table table;

	const uchar *buffer = static_cast<const uchar *>(data);

	quint32 crc = 0xffffffff;
	for (int i = 0; i != len; ++i) {
		crc = (crc >> 8) ^ table[(crc & 0xFF) ^ buffer[i]];
	}

	return static_cast<qint32>(crc ^ 0xffffffff);
}

char hexChar(uchar ch) {
	if (ch < 10) {
		return '0' + ch;
	} else if (ch < 16) {
		return 'a' + (ch - 10);
	}
	return '0';
}

char hexSecondChar(char ch) {
	return hexChar((*reinterpret_cast<uchar*>(&ch)) & 0x0F);
}

char hexFirstChar(char ch) {
	return hexChar((*reinterpret_cast<uchar*>(&ch)) >> 4);
}

QString stringToEncodedString(const std::string &str) {
	QString result, lineBreak = "\\\n";
	result.reserve(str.size() * 8);
	bool writingHexEscapedCharacters = false, startOnNewLine = false;
	int lastCutSize = 0;
	for (uchar ch : str) {
		if (result.size() - lastCutSize > 80) {
			startOnNewLine = true;
			result.append(lineBreak);
			lastCutSize = result.size();
		}
		if (ch == '\n') {
			writingHexEscapedCharacters = false;
			result.append("\\n");
		} else if (ch == '\t') {
			writingHexEscapedCharacters = false;
			result.append("\\t");
		} else if (ch == '"' || ch == '\\') {
			writingHexEscapedCharacters = false;
			result.append('\\').append(ch);
		} else if (ch < 32 || ch > 127) {
			writingHexEscapedCharacters = true;
			result.append("\\x").append(hexFirstChar(ch)).append(hexSecondChar(ch));
		} else {
			if (writingHexEscapedCharacters) {
				writingHexEscapedCharacters = false;
				result.append("\"\"");
			}
			result.append(ch);
		}
	}
	return '"' + (startOnNewLine ? lineBreak : QString()) + result + '"';
}

QString stringToBinaryArray(const std::string &str) {
	QStringList rows, chars;
	chars.reserve(13);
	rows.reserve(1 + (str.size() / 13));
	for (uchar ch : str) {
		if (chars.size() > 12) {
			rows.push_back(chars.join(", "));
			chars.clear();
		}
		chars.push_back(QString("0x") + hexFirstChar(ch) + hexSecondChar(ch));
	}
	if (!chars.isEmpty()) {
		rows.push_back(chars.join(", "));
	}
	return QString("{") + ((rows.size() > 1) ? '\n' : ' ') + rows.join(",\n") + " }";
}

QString pxValueName(int value) {
	QString result = "px";
	if (value < 0) {
		value = -value;
		result += 'm';
	}
	return result + QString::number(value);
}

QString moduleBaseName(const structure::Module &module) {
	auto moduleInfo = QFileInfo(module.filepath());
	auto moduleIsPalette = (moduleInfo.suffix() == "palette");
	return moduleIsPalette ? "palette" : "style_" + moduleInfo.baseName();
}

QString colorFallbackName(structure::Value value) {
	auto copy = value.copyOf();
	if (!copy.isEmpty()) {
		return copy.back();
	}
	return value.Color().fallback;
}

QChar paletteColorPart(uchar part) {
	part = (part & 0x0F);
	if (part >= 10) {
		return 'a' + (part - 10);
	}
	return '0' + part;
}

QString paletteColorComponent(uchar value) {
	return QString() + paletteColorPart(value >> 4) + paletteColorPart(value);
}

QString paletteColorValue(const structure::data::color &value) {
	auto result = paletteColorComponent(value.red) + paletteColorComponent(value.green) + paletteColorComponent(value.blue);
	if (value.alpha != 255) result += paletteColorComponent(value.alpha);
	return result;
}

} // namespace

Generator::Generator(const structure::Module &module, const QString &destBasePath, const common::ProjectInfo &project, bool isPalette)
: module_(module)
, basePath_(destBasePath)
, baseName_(QFileInfo(basePath_).baseName())
, project_(project)
, isPalette_(isPalette) {
}

bool Generator::writeHeader() {
	header_ = std::make_unique<common::CppFile>(basePath_ + ".h", project_);

	header_->include("ui/style/style_core.h").newline();

	if (!writeHeaderStyleNamespace()) {
		return false;
	}
	if (!writeRefsDeclarations()) {
		return false;
	}

	return header_->finalize();
}

bool Generator::writeSource() {
	source_ = std::make_unique<common::CppFile>(basePath_ + ".cpp", project_);

	writeIncludesInSource();

	if (module_.hasVariables()) {
		source_->pushNamespace().newline();
		source_->stream() << "\
bool inited = false;\n\
\n\
class Module_" << baseName_ << " : public style::internal::ModuleBase {\n\
public:\n\
	Module_" << baseName_ << "() { style::internal::registerModule(this); }\n\
	~Module_" << baseName_ << "() { style::internal::unregisterModule(this); }\n\
\n\
	void start() override {\n\
		style::internal::init_" << baseName_ << "();\n\
	}\n\
	void stop() override {\n\
	}\n\
};\n\
Module_" << baseName_ << " registrator;\n";
		if (isPalette_) {
			source_->newline();
			source_->stream() << "style::palette _palette;\n";
		} else {
			if (!writeVariableDefinitions()) {
				return false;
			}
		}
		source_->newline().popNamespace();

		source_->newline().pushNamespace("st");
		if (!writeRefsDefinition()) {
			return false;
		}

		source_->popNamespace().newline().pushNamespace("style");
		if (isPalette_) {
			writeSetPaletteColor();
		}
		source_->pushNamespace("internal").newline();
		if (!writeVariableInit()) {
			return false;
		}
	}

	return source_->finalize();
}

// Empty result means an error.
QString Generator::typeToString(structure::Type type) const {
	switch (type.tag) {
	case Tag::Invalid: return QString();
	case Tag::Int: return "int";
	case Tag::Double: return "double";
	case Tag::Pixels: return "int";
	case Tag::String: return "QString";
	case Tag::Color: return "style::color";
	case Tag::Point: return "style::point";
	case Tag::Size: return "style::size";
	case Tag::Cursor: return "style::cursor";
	case Tag::Align: return "style::align";
	case Tag::Margins: return "style::margins";
	case Tag::Font: return "style::font";
	case Tag::Icon: return "style::icon";
	case Tag::Struct: return "style::" + type.name.back();
	}
	return QString();
}

// Empty result means an error.
QString Generator::typeToDefaultValue(structure::Type type) const {
	switch (type.tag) {
	case Tag::Invalid: return QString();
	case Tag::Int: return "0";
	case Tag::Double: return "0.";
	case Tag::Pixels: return "0";
	case Tag::String: return "QString()";
	case Tag::Color: return "{ Qt::Uninitialized }";
	case Tag::Point: return "{ 0, 0 }";
	case Tag::Size: return "{ 0, 0 }";
	case Tag::Cursor: return "style::cur_default";
	case Tag::Align: return "style::al_topleft";
	case Tag::Margins: return "{ 0, 0, 0, 0 }";
	case Tag::Font: return "{ Qt::Uninitialized }";
	case Tag::Icon: return "{ Qt::Uninitialized }";
	case Tag::Struct: {
		if (auto realType = module_.findStruct(type.name)) {
			QStringList fields;
			for (auto field : realType->fields) {
				fields.push_back(typeToDefaultValue(field.type));
			}
			return "{ " + fields.join(", ") + " }";
		}
		return QString();
	} break;
	}
	return QString();
}

// Empty result means an error.
QString Generator::valueAssignmentCode(structure::Value value) const {
	auto copy = value.copyOf();
	if (!copy.isEmpty()) {
		auto result = "st::" + copy.back();

		// Copy is disabled for colors.
		if (value.type().tag == Tag::Color || value.type().tag == Tag::Struct) {
			result += ".clone()";
		}
		return result;
	}

	switch (value.type().tag) {
	case Tag::Invalid: return QString();
	case Tag::Int: return QString("%1").arg(value.Int());
	case Tag::Double: return QString("%1").arg(value.Double());
	case Tag::Pixels: return pxValueName(value.Int());
	case Tag::String: return QString("qsl(%1)").arg(stringToEncodedString(value.String()));
	case Tag::Color: {
		auto v(value.Color());
		return QString("{ %1, %2, %3, %4 }").arg(v.red).arg(v.green).arg(v.blue).arg(v.alpha);
	} break;
	case Tag::Point: {
		auto v(value.Point());
		return QString("{ %1, %2 }").arg(pxValueName(v.x)).arg(pxValueName(v.y));
	} break;
	case Tag::Size: {
		auto v(value.Size());
		return QString("{ %1, %2 }").arg(pxValueName(v.width)).arg(pxValueName(v.height));
	} break;
	case Tag::Cursor: return QString("style::cur_%1").arg(value.String().c_str());
	case Tag::Align: return QString("style::al_%1").arg(value.String().c_str());
	case Tag::Margins: {
		auto v(value.Margins());
		return QString("{ %1, %2, %3, %4 }").arg(pxValueName(v.left)).arg(pxValueName(v.top)).arg(pxValueName(v.right)).arg(pxValueName(v.bottom));
	} break;
	case Tag::Font: {
		auto v(value.Font());
		QString family = "0";
		if (!v.family.empty()) {
			auto familyIndex = fontFamilies_.value(v.family, -1);
			if (familyIndex < 0) {
				return QString();
			}
			family = QString("font%1index").arg(familyIndex);
		}
		return QString("{ %1, %2, %3 }").arg(pxValueName(v.size)).arg(v.flags).arg(family);
	} break;
	case Tag::Icon: {
		auto v(value.Icon());
		if (v.parts.empty()) return QString("{}");

		QStringList parts;
		for (const auto &part : v.parts) {
			auto maskIndex = iconMasks_.value(part.filename, -1);
			if (maskIndex < 0) {
				return QString();
			}
			auto color = valueAssignmentCode(part.color);
			auto offset = valueAssignmentCode(part.offset);
			parts.push_back(QString("MonoIcon{ &iconMask%1, %2, %3 }").arg(maskIndex).arg(color).arg(offset));
		}
		return QString("{ %1 }").arg(parts.join(", "));
	} break;
	case Tag::Struct: {
		if (!value.Fields()) return QString();

		QStringList fields;
		for (auto field : *value.Fields()) {
			fields.push_back(valueAssignmentCode(field.variable.value));
		}
		return "{ " + fields.join(", ") + " }";
	} break;
	}
	return QString();
}

bool Generator::writeHeaderStyleNamespace() {
	if (!module_.hasStructs() && !module_.hasVariables()) {
		return true;
	}
	header_->pushNamespace("style");

	if (module_.hasVariables()) {
		header_->pushNamespace("internal").newline();
		header_->stream() << "void init_" << baseName_ << "();\n\n";
		header_->popNamespace();
	}
	bool wroteForwardDeclarations = writeStructsForwardDeclarations();
	if (module_.hasStructs()) {
		if (!wroteForwardDeclarations) {
			header_->newline();
		}
		if (!writeStructsDefinitions()) {
			return false;
		}
	} else if (isPalette_) {
		if (!wroteForwardDeclarations) {
			header_->newline();
		}
		if (!writePaletteDefinition()) {
			return false;
		}
	}

	header_->popNamespace().newline();
	return true;
}

bool Generator::writePaletteDefinition() {
	header_->stream() << "\
class palette {\n\
public:\n\
	palette() = default;\n\
	palette(const palette &other) = delete;\n\
\n\
	QByteArray save() const;\n\
	bool load(const QByteArray &cache);\n\
	bool setColor(QLatin1String name, uchar r, uchar g, uchar b, uchar a);\n\
	bool setColor(QLatin1String name, QLatin1String from);\n\
\n\
	// Created not inited, should be finalized before usage.\n\
	void finalize();\n\
\n";

	int indexInPalette = 0;
	if (!module_.enumVariables([this, &indexInPalette](const Variable &variable) -> bool {
		auto name = variable.name.back();
		if (variable.value.type().tag != structure::TypeTag::Color) {
			return false;
		}

		auto index = (indexInPalette++);
		header_->stream() << "\tinline const color &" << name << "() const { return _colors[" << index << "]; };\n";
		return true;
	})) return false;

	auto count = indexInPalette;
	header_->stream() << "\
\n\
	palette &operator=(const palette &other) {\n\
		auto wasReady = _ready;\n\
		for (int i = 0; i != " << count << "; ++i) {\n\
			if (other._status[i] == Status::Loaded) {\n\
				if (_status[i] == Status::Initial) {\n\
					new (data(i)) internal::ColorData(*other.data(i));\n\
				} else {\n\
					*data(i) = *other.data(i);\n\
				}\n\
			} else if (_status[i] != Status::Initial) {\n\
				data(i)->~ColorData();\n\
				_status[i] = Status::Initial;\n\
				_ready = false;\n\
			}\n\
		}\n\
		if (wasReady && !_ready) {\n\
			finalize();\n\
		}\n\
		return *this;\n\
	}\n\
\n\
	static int32 Checksum();\n\
\n\
	~palette() {\n\
		for (int i = 0; i != " << count << "; ++i) {\n\
			if (_status[i] != Status::Initial) {\n\
				data(i)->~ColorData();\n\
			}\n\
		}\n\
	}\n\
\n\
private:\n\
	struct TempColorData { uchar r, g, b, a; };\n\
	void compute(int index, int fallbackIndex, TempColorData value) {\n\
		if (_status[index] == Status::Initial) {\n\
			if (fallbackIndex >= 0 && _status[fallbackIndex] != Status::Initial) {\n\
				_status[index] = Status::Loaded;\n\
				new (data(index)) internal::ColorData(*data(fallbackIndex));\n\
			} else {\n\
				_status[index] = Status::Created;\n\
				new (data(index)) internal::ColorData(value.r, value.g, value.b, value.a);\n\
			}\n\
		}\n\
	}\n\
\n\
	internal::ColorData *data(int index) {\n\
		return reinterpret_cast<internal::ColorData*>(_data) + index;\n\
	}\n\
\n\
	const internal::ColorData *data(int index) const {\n\
		return reinterpret_cast<const internal::ColorData*>(_data) + index;\n\
	}\n\
\n\
	void setData(int index, const internal::ColorData &value) {\n\
		if (_status[index] == Status::Initial) {\n\
			new (data(index)) internal::ColorData(value);\n\
		} else {\n\
			*data(index) = value;\n\
		}\n\
		_status[index] = Status::Loaded;\n\
	}\n\
\n\
	enum class Status {\n\
		Initial,\n\
		Created,\n\
		Loaded,\n\
	};\n\
\n\
	alignas(alignof(internal::ColorData)) char _data[sizeof(internal::ColorData) * " << count << "];\n\
\n\
	color _colors[" << count << "] = {\n";
	for (int i = 0; i != count; ++i) {
		header_->stream() << "\t\tdata(" << i << "),\n";
	}
	header_->stream() << "\
	};\n\
	Status _status[" << count << "] = { Status::Initial };\n\
	bool _ready = false;\n\
\n\
};\n\
\n\
namespace main_palette {\n\
\n\
QByteArray save();\n\
bool load(const QByteArray &cache);\n\
bool setColor(QLatin1String name, uchar r, uchar g, uchar b, uchar a);\n\
bool setColor(QLatin1String name, QLatin1String from);\n\
void apply(const palette &other);\n\
\n\
} // namespace main_palette\n";

	header_->newline();

	return true;
}

bool Generator::writeStructsForwardDeclarations() {
	bool hasNoExternalStructs = module_.enumVariables([this](const Variable &value) -> bool {
		if (value.value.type().tag == structure::TypeTag::Struct) {
			if (!module_.findStructInModule(value.value.type().name, module_)) {
				return false;
			}
		}
		return true;
	});
	if (hasNoExternalStructs) {
		return false;
	}

	header_->newline();
	bool result = module_.enumVariables([this](const Variable &value) -> bool {
		if (value.value.type().tag == structure::TypeTag::Struct) {
			if (!module_.findStructInModule(value.value.type().name, module_)) {
				header_->stream() << "struct " << value.value.type().name.back() << ";\n";
			}
		}
		return true;
	});
	header_->newline();
	return result;
}

bool Generator::writeStructsDefinitions() {
	if (!module_.hasStructs()) {
		return true;
	}

	bool result = module_.enumStructs([this](const Struct &value) -> bool {
		QStringList fields;
		for (auto field : value.fields) {
			auto clone = field.name.back();
			if (field.type.tag == Tag::Color || field.type.tag == Tag::Struct) {
				clone += ".clone()";
			}
			fields.push_back(clone);
		}
		header_->stream() << "\
struct " << value.name.back() << " {\n\
	" << value.name.back() << " clone() const {\n\
		return { " << fields.join(", ") << " };\n\
	}\n";
		if (!fields.empty()) header_->newline();

		for (auto &field : value.fields) {
			auto type = typeToString(field.type);
			if (type.isEmpty()) {
				return false;
			}
			header_->stream() << "\t" << type << " " << field.name.back() << ";\n";
		}
		header_->stream() << "\
};\n\n";
		return true;
	});

	return result;
}

bool Generator::writeRefsDeclarations() {
	if (!module_.hasVariables()) {
		return true;
	}

	header_->pushNamespace("st");

	bool result = module_.enumVariables([this](const Variable &value) -> bool {
		auto name = value.name.back();
		auto type = typeToString(value.value.type());
		if (type.isEmpty()) {
			return false;
		}

		header_->stream() << "extern const " << type << " &" << name << ";\n";
		return true;
	});

	header_->popNamespace();

	return result;
}

bool Generator::writeIncludesInSource() {
	if (!module_.hasIncludes()) {
		return true;
	}

	bool result = module_.enumIncludes([this](const Module &module) -> bool {
		source_->include(moduleBaseName(module) + ".h");
		return true;
	});
	source_->newline();
	return result;
}

bool Generator::writeVariableDefinitions() {
	if (!module_.hasVariables()) {
		return true;
	}

	source_->newline();
	bool result = module_.enumVariables([this](const Variable &variable) -> bool {
		auto name = variable.name.back();
		auto type = typeToString(variable.value.type());
		if (type.isEmpty()) {
			return false;
		}
		source_->stream() << type << " _" << name << " = " << typeToDefaultValue(variable.value.type()) << ";\n";
		return true;
	});
	return result;
}

bool Generator::writeRefsDefinition() {
	if (!module_.hasVariables()) {
		return true;
	}

	bool result = module_.enumVariables([this](const Variable &variable) -> bool {
		auto name = variable.name.back();
		auto type = typeToString(variable.value.type());
		if (type.isEmpty()) {
			return false;
		}
		source_->stream() << "const " << type << " &" << name << "(";
		if (isPalette_) {
			source_->stream() << "_palette." << name << "()";
		} else {
			source_->stream() << "_" << name;
		}
		source_->stream() << ");\n";
		return true;
	});
	return result;
}

bool Generator::writeSetPaletteColor() {
	source_->newline();
	source_->stream() << "\
void palette::finalize() {\n\
	if (_ready) return;\n\
	_ready = true;\n\n";

	int indexInPalette = 0;
	QByteArray checksumString;
	bool result = module_.enumVariables([this, &indexInPalette, &checksumString](const Variable &variable) -> bool {
		auto name = variable.name.back();
		auto index = indexInPalette++;
		paletteIndices_[name] = index;
		if (variable.value.type().tag != structure::TypeTag::Color) {
			return false;
		}
		auto color = variable.value.Color();
		auto fallbackIndex = paletteIndices_.value(colorFallbackName(variable.value), -1);
		source_->stream() << "\tcompute(" << index << ", " << fallbackIndex << ", {" << color.red << ", " << color.green << ", " << color.blue << ", " << color.alpha << "});\n";
		checksumString.append('&' + name + ':' + valueAssignmentCode(variable.value));
		return true;
	});
	auto count = indexInPalette;
	auto checksum = hashCrc32(checksumString.constData(), checksumString.size());

	source_->stream() << "\
}\n\
\n\
int32 palette::Checksum() {\n\
	return " << checksum << ";\n\
}\n";

	source_->newline().pushNamespace().newline();
	source_->stream() << "\
int getPaletteIndex(QLatin1String name) {\n\
	auto size = name.size();\n\
	auto data = name.data();\n";

	int already = 0;
	QString prefix;
	QString tabs;
	for (auto i = paletteIndices_.end(), b = paletteIndices_.begin(); i != b;) {
		--i;
		auto name = i.key();
		auto index = i.value();
		auto prev = i;
		auto next = (i == b) ? QString() : (--prev).key();
		while ((prefix.size() > name.size()) || (!prefix.isEmpty() && prefix.mid(0, already - 1) != name.mid(0, already - 1))) {
			source_->stream() << "\n" << tabs << "};";
			prefix.chop(1);
			tabs.chop(1);
			--already;
		}
		if (!prefix.isEmpty() && prefix[already - 1] != name[already - 1]) {
			source_->stream() << "\n" << tabs << "case '" << name[already - 1] << "':";
			prefix[already - 1] = name[already - 1];
		}
		while (name.size() > already) {
			if (name.mid(0, already) != next.mid(0, already)) {
				break;
			} else if (next.size() <= already) {
				source_->stream() << "\n" << tabs << "\tif (size == " << name.size() << ")";
				break;
			}
			source_->stream() << "\n" << tabs << "\tif (size > " << already << ") switch (data[" << already << "]) {\n";
			prefix.append(name[already]);
			tabs.append('\t');
			++already;
			source_->stream() << tabs << "case '" << name[already - 1] << "':";
		}
		if (name.size() == already || name.mid(0, already) != next.mid(0, already)) {
			source_->stream() << " return (size == " << name.size();
			if (name.size() != already) {
				source_->stream() << " && ";
			}
		} else {
			source_->stream() << " return (";
		}
		if (already != name.size()) {
			source_->stream() << "!memcmp(data + " << already << ", \"" << name.mid(already) << "\", " << (name.size() - already) << ")";
		}
		source_->stream() << ") ? " << index << " : -1;";
	}
	while (!prefix.isEmpty()) {
		source_->stream() << "\n" << tabs << "};";
		prefix.chop(1);
		tabs.chop(1);
		--already;
	}

	source_->stream() << "\
\n\
	return -1;\n\
}\n";

	source_->newline().popNamespace().newline();
	source_->stream() << "\
QByteArray palette::save() const {\n\
	if (!_ready) const_cast<palette*>(this)->finalize();\n\
\n\
	auto result = QByteArray(" << (count * 4) << ", Qt::Uninitialized);\n\
	for (auto i = 0, index = 0; i != " << count << "; ++i) {\n\
		result[index++] = static_cast<uchar>(data(i)->c.red());\n\
		result[index++] = static_cast<uchar>(data(i)->c.green());\n\
		result[index++] = static_cast<uchar>(data(i)->c.blue());\n\
		result[index++] = static_cast<uchar>(data(i)->c.alpha());\n\
	}\n\
	return result;\n\
}\n\
\n\
bool palette::load(const QByteArray &cache) {\n\
	if (cache.size() != " << (count * 4) << ") return false;\n\
\n\
	auto p = reinterpret_cast<const uchar*>(cache.constData());\n\
	for (auto i = 0; i != " << count << "; ++i) {\n\
		setData(i, { p[i * 4 + 0], p[i * 4 + 1], p[i * 4 + 2], p[i * 4 + 3] });\n\
	}\n\
	return true;\n\
}\n\
\n\
bool palette::setColor(QLatin1String name, uchar r, uchar g, uchar b, uchar a) {\n\
	auto index = getPaletteIndex(name);\n\
	if (index >= 0) {\n\
		setData(index, { r, g, b, a });\n\
		return true;\n\
	}\n\
	return false;\n\
}\n\
\n\
bool palette::setColor(QLatin1String name, QLatin1String from) {\n\
	auto nameIndex = getPaletteIndex(name);\n\
	auto fromIndex = getPaletteIndex(from);\n\
	if (nameIndex >= 0 && fromIndex >= 0 && _status[fromIndex] == Status::Loaded) {\n\
		setData(nameIndex, *data(fromIndex));\n\
		return true;\n\
	}\n\
	return false;\n\
}\n\
\n\
namespace main_palette {\n\
\n\
QByteArray save() {\n\
	return _palette.save();\n\
}\n\
\n\
bool load(const QByteArray &cache) {\n\
	if (_palette.load(cache)) {\n\
		style::internal::resetIcons();\n\
		return true;\n\
	}\n\
	return false;\n\
}\n\
\n\
bool setColor(QLatin1String name, uchar r, uchar g, uchar b, uchar a) {\n\
	return _palette.setColor(name, r, g, b, a);\n\
}\n\
\n\
bool setColor(QLatin1String name, QLatin1String from) {\n\
	return _palette.setColor(name, from);\n\
}\n\
\n\
void apply(const palette &other) {\n\
	_palette = other;\n\
	style::internal::resetIcons();\n\
}\n\
\n\
} // namespace main_palette\n\
\n";

	return result;
}

bool Generator::writeVariableInit() {
	if (!module_.hasVariables()) {
		return true;
	}

	if (!collectUniqueValues()) {
		return false;
	}
	bool hasUniqueValues = (!pxValues_.isEmpty() || !fontFamilies_.isEmpty() || !iconMasks_.isEmpty());
	if (hasUniqueValues) {
		source_->pushNamespace();
		if (!writePxValuesInit()) {
			return false;
		}
		if (!writeFontFamiliesInit()) {
			return false;
		}
		if (!writeIconValues()) {
			return false;
		}
		source_->popNamespace().newline();
	}

	source_->stream() << "\
void init_" << baseName_ << "() {\n\
	if (inited) return;\n\
	inited = true;\n\n";

	if (module_.hasIncludes()) {
		bool writtenAtLeastOne = false;
		bool result = module_.enumIncludes([this,&writtenAtLeastOne](const Module &module) -> bool {
			if (module.hasVariables()) {
				source_->stream() << "\tinit_" + moduleBaseName(module) + "();\n";
				writtenAtLeastOne = true;
			}
			return true;
		});
		if (!result) {
			return false;
		}
		if (writtenAtLeastOne) {
			source_->newline();
		}
	}

	if (!pxValues_.isEmpty() || !fontFamilies_.isEmpty()) {
		if (!pxValues_.isEmpty()) {
			source_->stream() << "\tinitPxValues();\n";
		}
		if (!fontFamilies_.isEmpty()) {
			source_->stream() << "\tinitFontFamilies();\n";
		}
		source_->newline();
	}

	if (isPalette_) {
		source_->stream() << "\t_palette.finalize();\n";
	} else if (!module_.enumVariables([this](const Variable &variable) -> bool {
		auto name = variable.name.back();
		auto value = valueAssignmentCode(variable.value);
		if (value.isEmpty()) {
			return false;
		}
		source_->stream() << "\t_" << name << " = " << value << ";\n";
		return true;
	})) {
		return false;
	}
	source_->stream() << "\
}\n\n";
	return true;
}

bool Generator::writePxValuesInit() {
	if (pxValues_.isEmpty()) {
		return true;
	}

	for (auto i = pxValues_.cbegin(), e = pxValues_.cend(); i != e; ++i) {
		source_->stream() << "int " << pxValueName(i.key()) << " = " << i.key() << ";\n";
	}
	source_->stream() << "\
void initPxValues() {\n\
	if (cRetina()) return;\n\
\n\
	switch (cScale()) {\n";
	for (int i = 1, scalesCount = scales.size(); i < scalesCount; ++i) {
		source_->stream() << "\tcase " << scaleNames.at(i) << ":\n";
		for (auto it = pxValues_.cbegin(), e = pxValues_.cend(); it != e; ++it) {
			auto value = it.key();
			int adjusted = structure::data::pxAdjust(value, scales.at(i));
			if (adjusted != value) {
				source_->stream() << "\t\t" << pxValueName(value) << " = " << adjusted << ";\n";
			}
		}
		source_->stream() << "\tbreak;\n";
	}
	source_->stream() << "\
	}\n\
}\n\n";
	return true;
}

bool Generator::writeFontFamiliesInit() {
	if (fontFamilies_.isEmpty()) {
		return true;
	}

	for (auto familyIndex : fontFamilies_) {
		source_->stream() << "int font" << familyIndex << "index;\n";
	}
	source_->stream() << "void initFontFamilies() {\n";
	for (auto i = fontFamilies_.cbegin(), e = fontFamilies_.cend(); i != e; ++i) {
		auto family = stringToEncodedString(i.key());
		source_->stream() << "\tfont" << i.value() << "index = style::internal::registerFontFamily(" << family << ");\n";
	}
	source_->stream() << "}\n\n";
	return true;
}

namespace {

QByteArray iconMaskValueSize(int width, int height) {
	QByteArray result;
	QLatin1String generateTag("GENERATE:");
	result.append(generateTag.data(), generateTag.size());
	QLatin1String sizeTag("SIZE:");
	result.append(sizeTag.data(), sizeTag.size());
	{
		QBuffer buffer(&result);
		buffer.open(QIODevice::Append);

		QDataStream stream(&buffer);
		stream.setVersion(QDataStream::Qt_5_1);
		stream << qint32(width) << qint32(height);
	}
	return result;
}

QByteArray iconMaskValuePng(QString filepath) {
	QByteArray result;

	auto pathAndModifiers = filepath.split('-');
	filepath = pathAndModifiers[0];
	auto modifiers = pathAndModifiers.mid(1);

	QImage png100x(filepath + ".png");
	QImage png200x(filepath + "@2x.png");
	png100x.setDevicePixelRatio(1.);
	png200x.setDevicePixelRatio(1.);
	if (png100x.isNull()) {
		common::logError(common::kErrorFileNotOpened, filepath + ".png") << "could not open icon file";
		return result;
	}
	if (png200x.isNull()) {
		common::logError(common::kErrorFileNotOpened, filepath + "@2x.png") << "could not open icon file";
		return result;
	}
	if (png100x.format() != png200x.format()) {
		common::logError(kErrorBadIconFormat, filepath + ".png") << "1x and 2x icons have different format";
		return result;
	}
	if (png100x.width() * 2 != png200x.width() || png100x.height() * 2 != png200x.height()) {
		common::logError(kErrorBadIconSize, filepath + ".png") << "bad icons size, 1x: " << png100x.width() << "x" << png100x.height() << ", 2x: " << png200x.width() << "x" << png200x.height();
		return result;
	}
	for (auto modifierName : modifiers) {
		if (auto modifier = GetModifier(modifierName)) {
			modifier(png100x, png200x);
		} else {
			common::logError(common::kErrorInternal, filepath) << "modifier should be valid here, name: " << modifierName.toStdString();
			return result;
		}
	}
	QImage png125x = png200x.scaled(structure::data::pxAdjust(png100x.width(), 5), structure::data::pxAdjust(png100x.height(), 5), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
	QImage png150x = png200x.scaled(structure::data::pxAdjust(png100x.width(), 6), structure::data::pxAdjust(png100x.height(), 6), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

	QImage composed(png200x.width() + png100x.width(), png200x.height() + png150x.height(), png100x.format());
	{
		QPainter p(&composed);
		p.setCompositionMode(QPainter::CompositionMode_Source);
		p.fillRect(0, 0, composed.width(), composed.height(), QColor(0, 0, 0, 255));
		p.drawImage(0, 0, png200x);
		p.drawImage(png200x.width(), 0, png100x);
		p.drawImage(0, png200x.height(), png150x);
		p.drawImage(png150x.width(), png200x.height(), png125x);
	}
	{
		QBuffer buffer(&result);
		composed.save(&buffer, "PNG");
	}
	return result;
}

} // namespace

bool Generator::writeIconValues() {
	if (iconMasks_.isEmpty()) {
		return true;
	}

	for (auto i = iconMasks_.cbegin(), e = iconMasks_.cend(); i != e; ++i) {
		QString filePath = i.key();
		QByteArray maskData;
		QImage png100x, png200x;
		if (filePath.startsWith("size://")) {
			QStringList dimensions = filePath.mid(7).split(',');
			if (dimensions.size() < 2 || dimensions.at(0).toInt() <= 0 || dimensions.at(1).toInt() <= 0) {
				common::logError(common::kErrorFileNotOpened, filePath) << "bad dimensions";
				return false;
			}
			maskData = iconMaskValueSize(dimensions.at(0).toInt(), dimensions.at(1).toInt());
		} else {
			maskData = iconMaskValuePng(filePath);
		}
		if (maskData.isEmpty()) {
			return false;
		}
		source_->stream() << "const uchar iconMask" << i.value() << "Data[] = " << stringToBinaryArray(std::string(maskData.constData(), maskData.size())) << ";\n";
		source_->stream() << "IconMask iconMask" << i.value() << "(iconMask" << i.value() << "Data);\n\n";
	}
	return true;
}

bool Generator::collectUniqueValues() {
	int fontFamilyIndex = 0;
	int iconMaskIndex = 0;
	std::function<bool(const Variable&)> collector = [this, &collector, &fontFamilyIndex, &iconMaskIndex](const Variable &variable) {
		auto value = variable.value;
		if (!value.copyOf().isEmpty()) {
			return true;
		}

		switch (value.type().tag) {
		case Tag::Invalid:
		case Tag::Int:
		case Tag::Double:
		case Tag::String:
		case Tag::Color:
		case Tag::Cursor:
		case Tag::Align: break;
		case Tag::Pixels: pxValues_.insert(value.Int(), true); break;
		case Tag::Point: {
			auto v(value.Point());
			pxValues_.insert(v.x, true);
			pxValues_.insert(v.y, true);
		} break;
		case Tag::Size: {
			auto v(value.Size());
			pxValues_.insert(v.width, true);
			pxValues_.insert(v.height, true);
		} break;
		case Tag::Margins: {
			auto v(value.Margins());
			pxValues_.insert(v.left, true);
			pxValues_.insert(v.top, true);
			pxValues_.insert(v.right, true);
			pxValues_.insert(v.bottom, true);
		} break;
		case Tag::Font: {
			auto v(value.Font());
			pxValues_.insert(v.size, true);
			if (!v.family.empty() && !fontFamilies_.contains(v.family)) {
				fontFamilies_.insert(v.family, ++fontFamilyIndex);
			}
		} break;
		case Tag::Icon: {
			auto v(value.Icon());
			for (auto &part : v.parts) {
				pxValues_.insert(part.offset.Point().x, true);
				pxValues_.insert(part.offset.Point().y, true);
				if (!iconMasks_.contains(part.filename)) {
					iconMasks_.insert(part.filename, ++iconMaskIndex);
				}
			}
		} break;
		case Tag::Struct: {
			auto fields = variable.value.Fields();
			if (!fields) {
				return false;
			}

			for (auto field : *fields) {
				if (!collector(field.variable)) {
					return false;
				}
			}
		} break;
		}
		return true;
	};
	return module_.enumVariables(collector);
}

bool Generator::writeSampleTheme(const QString &filepath) {
	QByteArray content;
	QTextStream stream(&content);

	stream << "\
//\n\
// This is a sample Telegram Desktop theme file.\n\
// It was generated from the 'colors.palette' style file.\n\
//\n\
// To create a theme with a background image included you should\n\
// put two files in a .zip archive:\n\
//\n\
// First one is the color scheme like the one you're viewing\n\
// right now, this file should be named 'colors.tdesktop-theme'.\n\
//\n\
// Second one should be the background image and it can be named\n\
// 'background.jpg', 'background.png', 'tiled.jpg' or 'tiled.png'.\n\
// You should name it 'background' (if you'd like it not to be tiled),\n\
// or it can be named 'tiled' (if you'd like it to be tiled).\n\
//\n\
// After that you need to change the extension of your .zip archive\n\
// to 'tdesktop-theme', so you'll have:\n\
//\n\
// mytheme.tdesktop-theme\n\
// |-colors.tdesktop-theme\n\
// |-background.jpg (or tiled.jpg, background.png, tiled.png)\n\
//\n\n";

	QList<structure::FullName> names;
	module_.enumVariables([this, &names](const Variable &variable) -> bool {
		names.push_back(variable.name);
		return true;
	});
	bool result = module_.enumVariables([this, &names, &stream](const Variable &variable) -> bool {
		auto name = variable.name.back();
		if (variable.value.type().tag != structure::TypeTag::Color) {
			return false;
		}
		auto color = variable.value.Color();
		auto colorString = paletteColorValue(color);
		auto fallbackIndex = paletteIndices_.value(colorFallbackName(variable.value), -1);
		if (fallbackIndex >= 0) {
			auto fallbackVariable = module_.findVariableInModule(names[fallbackIndex], module_);
			if (!fallbackVariable || fallbackVariable->value.type().tag != structure::TypeTag::Color) {
				return false;
			}
			auto fallbackName = fallbackVariable->name.back();
			auto fallbackColor = fallbackVariable->value.Color();
			if (colorString == paletteColorValue(fallbackColor)) {
				stream << name << ": " << fallbackName << ";\n";
			} else {
				stream << name << ": #" << colorString << "; // " << fallbackName << ";\n";
			}
		} else {
			stream << name << ": #" << colorString << ";\n";
		}
		return true;
	});
	if (!result) {
		return result;
	}

	stream.flush();

	QFile file(filepath);
	if (file.open(QIODevice::ReadOnly)) {
		if (file.readAll() == content) {
			file.close();
			return true;
		}
		file.close();
	}

	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
	if (file.write(content) != content.size()) {
		return false;
	}
	return true;
}

} // namespace style
} // namespace codegen
