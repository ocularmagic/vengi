/**
 * @file
 */

#include "app/tests/AbstractTest.h"
#include "video/Shader.h"
#include "video/ShaderTypes.h"
#include "io/Filesystem.h"
#include "core/StringUtil.h"
#include "core/Var.h"

namespace video {

class ShaderTest : public app::AbstractTest {
};

TEST_F(ShaderTest, testInclude) {
	const io::FilesystemPtr& filesystem = _testApp->filesystem();

	filesystem->homeWrite("foobar.vert", "#define SUCCESS");
	filesystem->homeWrite("foobar.frag", "#define SUCCESS");

	Shader s;
	const core::String &vert = s.getSource(ShaderType::Vertex, "#include \"foobar.vert\"");
	const core::String &frag = s.getSource(ShaderType::Fragment, "#include \"foobar.frag\"");
	ASSERT_TRUE(core::string::contains(vert, "SUCCESS")) << "vertex shader: " << vert;
	ASSERT_TRUE(core::string::contains(frag, "SUCCESS")) << "fragment shader: " << frag;
}

#ifdef USE_OPENGLES
TEST_F(ShaderTest, testStripStandaloneLayoutBinding) {
	Shader::glslVersion = 300;
	Shader shader;
	const core::String &source = shader.getSource(ShaderType::Fragment,
			"layout(binding = 2) uniform sampler2D u_texture;\n"
			"layout(std140, binding = 1) uniform u_block { vec4 color; };\n");
	EXPECT_FALSE(core::string::contains(source, "layout()")) << source;
	EXPECT_TRUE(core::string::contains(source, "uniform sampler2D u_texture;")) << source;
	EXPECT_TRUE(core::string::contains(source, "layout(std140) uniform u_block")) << source;
}
#endif

TEST_F(ShaderTest, testUniformBlockBindingLookup) {
	const video::ShaderResourceBinding bindings[] = {
		{1, video::ShaderResourceBinding::UniformBuffer, 3, "u_frag"},
		{0, video::ShaderResourceBinding::UniformBuffer, 3, "u_vert"},
		{2, video::ShaderResourceBinding::CombinedImageSampler, 2, nullptr},
	};
	EXPECT_EQ(1, video::shaderResourceUniformBlockBinding(bindings, 3, "u_frag"));
	EXPECT_EQ(0, video::shaderResourceUniformBlockBinding(bindings, 3, "u_vert"));
	EXPECT_EQ(-1, video::shaderResourceUniformBlockBinding(bindings, 3, "u_missing"));
	EXPECT_EQ(-1, video::shaderResourceUniformBlockBinding(bindings, 3, nullptr));
	EXPECT_EQ(-1, video::shaderResourceUniformBlockBinding(nullptr, 3, "u_frag"));
}

}
