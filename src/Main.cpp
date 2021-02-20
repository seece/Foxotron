#include <stdio.h>
#include <algorithm>

#include "Geometry.h"

#define IMGUI_IMPL_OPENGL_LOADER_GLEW
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include "ImGuiFileBrowser.h"

#include "ext.hpp"
#include "gtx/rotate_vector.hpp"

#include <jsonxx.h>

Renderer::Shader * LoadShader( const char * vsPath, const char * fsPath )
{
  char vertexShader[ 16 * 1024 ] = { 0 };
  FILE * fileVS = fopen( vsPath, "rb" );
  if ( !fileVS )
  {
    printf( "Vertex shader load failed: '%s'\n", vsPath );
    return NULL;
  }
  fread( vertexShader, 1, 16 * 1024, fileVS );
  fclose( fileVS );

  char fragmentShader[ 16 * 1024 ] = { 0 };
  FILE * fileFS = fopen( fsPath, "rb" );
  if ( !fileFS )
  {
    printf( "Fragment shader load failed: '%s'\n", fsPath );
    return NULL;
  }
  fread( fragmentShader, 1, 16 * 1024, fileFS );
  fclose( fileFS );

  char error[ 4096 ];
  Renderer::Shader * shader = Renderer::CreateShader( vertexShader, (int) strlen( vertexShader ), fragmentShader, (int) strlen( fragmentShader ), error, 4096 );
  if ( !shader )
  {
    printf( "Shader build failed: %s\n", error );
  }

  return shader;
}

const jsonxx::Object * gCurrentShaderConfig = NULL;
Renderer::Shader * gCurrentShader = NULL;
bool LoadShaderConfig( const jsonxx::Object * _shader )
{
  Renderer::Shader * newShader = LoadShader( _shader->get<jsonxx::String>( "vertexShader" ).c_str(), _shader->get<jsonxx::String>( "fragmentShader" ).c_str() );
  if ( !newShader )
  {
    return false;
  }

  gCurrentShaderConfig = _shader;

  if ( gCurrentShader )
  {
    Renderer::ReleaseShader( gCurrentShader );
    delete gCurrentShader;
  }
  gCurrentShader = newShader;

  return true;
}

glm::vec3 gCameraTarget( 0.0f, 0.0f, 0.0f );
float gCameraDistance = 500.0f;
Geometry gModel;

bool LoadMesh( const char * path )
{
  if ( !gModel.LoadMesh( path ) )
  {
    return false;
  }

  gModel.RebindVertexArray( gCurrentShader );

  gCameraTarget = ( gModel.mAABBMin + gModel.mAABBMax ) / 2.0f;
  gCameraDistance = glm::length( gCameraTarget - gModel.mAABBMin ) * 4.0f;

  return true;
}

void ShowNodeInImGui( int _parentID )
{
  for ( std::map<int, Geometry::Node>::iterator it = gModel.mNodes.begin(); it != gModel.mNodes.end(); it++ )
  {
    if ( it->second.mParentID == _parentID )
    {
      ImGui::Text( "%s", it->second.mName.c_str() );
      ImGui::Indent();
      for ( int i = 0; i < it->second.mMeshes.size(); i++ )
      {
        const Geometry::Mesh & mesh = gModel.mMeshes[ it->second.mMeshes[ i ] ];
        ImGui::TextColored( ImVec4( 1.0f, 0.5f, 1.0f, 1.0f ), "Mesh %d: %d vertices, %d triangles", i + 1, mesh.mVertexCount, mesh.mTriangleCount );
      }

      ShowNodeInImGui( it->second.mID );
      ImGui::Unindent();
    }
  }
}

void ShowMaterialInImGui( const char * _channel, Renderer::Texture * _texture )
{
  if ( !_texture )
  {
    return;
  }
  if ( ImGui::BeginTabItem( _channel ) )
  {
    ImGui::Text( "Texture: %s", _texture->mFilename.c_str() );
    ImGui::Text( "Dimensions: %d x %d", _texture->mWidth, _texture->mHeight );
    ImGui::Image( (void *) (intptr_t) _texture->mGLTextureID, ImVec2( 512.0f, 512.0f ) );
    ImGui::EndTabItem();
  }
}

Renderer::Texture* gBrdfLookupTable = NULL;

void loadBrdfLookupTable()
{
  const int width = 256;
  const int height = 256;
  const int comp = 2;
  const char* filename = "Skyboxes/brdf256.bin";

  if ( gBrdfLookupTable )
  {
    Renderer::ReleaseTexture( gBrdfLookupTable );
    delete gBrdfLookupTable;
    gBrdfLookupTable = NULL;
  }

  gBrdfLookupTable = Renderer::CreateRG32FTextureFromRawFile( filename, width, height );

  if ( !gBrdfLookupTable )
  {
    printf( "Couldn't load %dx%d BRDF lookup table '%s'!\n", width, height, filename );
  }
}


struct SkyImages
{
  Renderer::Texture* reflection = NULL;
  Renderer::Texture* env = NULL;
};

SkyImages gSkyImages;

void loadSkyImages( const char* reflectionPath, const char* envPath )
{
  if ( gSkyImages.reflection )
  {
    Renderer::ReleaseTexture( gSkyImages.reflection );
    gSkyImages.reflection = NULL;
  }
  gSkyImages.reflection = Renderer::CreateRGBA8TextureFromFile( reflectionPath );

  if ( gSkyImages.reflection )
  {
      glBindTexture( GL_TEXTURE_2D, gSkyImages.reflection->mGLTextureID );

      glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT );
      glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP );
  }

  if ( gSkyImages.env )
  {
    Renderer::ReleaseTexture( gSkyImages.env );
    gSkyImages.env = NULL;
  }

  if ( envPath )
  {
    gSkyImages.env = Renderer::CreateRGBA8TextureFromFile( envPath );

    if ( gSkyImages.env )
    {
      glBindTexture( GL_TEXTURE_2D, gSkyImages.env->mGLTextureID );

      glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT );
      glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP );
    }
    else
    {
      printf( "Couldn't load environment map '%s'!\n", envPath );
    }
  }
}

int main( int argc, const char * argv[] )
{
  jsonxx::Object options;
  FILE * configFile = fopen( "config.json", "rb" );
  if ( !configFile )
  {
    printf( "Config file not found!\n" );
    return -10;
  }

  char configData[ 65535 ];
  memset( configData, 0, 65535 );
  fread( configData, 1, 65535, configFile );
  fclose( configFile );

  options.parse( configData );
  if ( !options.has<jsonxx::Array>( "shaders" ) || !options.has<jsonxx::Array>( "skyImages" ) )
  {
    printf( "Config file broken!\n" );
    return -11;
  }

  //////////////////////////////////////////////////////////////////////////
  // Init renderer
  RENDERER_SETTINGS settings;
  settings.bVsync = false;
  settings.nWidth = 1280;
  settings.nHeight = 720;
  settings.windowMode = RENDERER_WINDOWMODE_WINDOWED;
  if ( !Renderer::Open( &settings ) )
  {
    printf( "Renderer::Open failed\n" );
    return -1;
  }

  //////////////////////////////////////////////////////////////////////////
  // Start up ImGui
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO & io = ImGui::GetIO();

  ImGui::StyleColorsDark();

  ImGui_ImplGlfw_InitForOpenGL( Renderer::mWindow, true );
  ImGui_ImplOpenGL3_Init();

  imgui_addons::ImGuiFileBrowser file_dialog;

  //////////////////////////////////////////////////////////////////////////
  // Bootstrap
  if ( !LoadShaderConfig( &options.get<jsonxx::Array>( "shaders" ).get<jsonxx::Object>( 0 ) ) )
  {
    return -4;
  }

  if ( argc >= 2 )
  {
    LoadMesh( argv[ 1 ] );
  }

  //////////////////////////////////////////////////////////////////////////
  // Mainloop
  bool appWantsToQuit = false;
  bool automaticCamera = false;
  glm::mat4x4 viewMatrix;
  glm::mat4x4 projectionMatrix;
  bool movingCamera = false;
  bool movingLight = false;
  float cameraYaw = 0.0f;
  float cameraPitch = 0.0f;
  float lightYaw = 0.0f;
  float lightPitch = 0.0f;
  float mouseClickPosX = 0.0f;
  float mouseClickPosY = 0.0f;
  glm::vec4 clearColor( 0.5f, 0.5f, 0.5f, 1.0f );
  std::string supportedExtensions = Geometry::GetSupportedExtensions();
  float skysphereOpacity = 1.0f;
  float skysphereBlur = 0.75f;

  bool xzySpace = false;
  const glm::mat4x4 xzyMatrix(
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f,-1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f );

  auto firstSkyImages = options.get<jsonxx::Array>( "skyImages" ).get<jsonxx::Object>( 0 );
  loadSkyImages(
    firstSkyImages.get<jsonxx::String>( "reflection" ).c_str(),
    firstSkyImages.has<jsonxx::String>( "env" ) ? firstSkyImages.get<jsonxx::String>( "env" ).c_str() : NULL );

  loadBrdfLookupTable();

  Geometry skysphere;
  skysphere.LoadMesh( "Skyboxes/skysphere.fbx" );

  Renderer::Shader * skysphereShader = LoadShader( "Skyboxes/skysphere.vs", "Skyboxes/skysphere.fs" );
  if ( !skysphereShader )
  {
    return -8;
  }
  skysphere.RebindVertexArray( skysphereShader );

  while ( !Renderer::WantsToQuit() && !appWantsToQuit )
  {
    Renderer::StartFrame( clearColor );

    //////////////////////////////////////////////////////////////////////////
    // ImGui windows etc.
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    bool openFileDialog = false;
    static bool showModelInfo = false;
    if ( ImGui::BeginMainMenuBar() )
    {
      if ( ImGui::BeginMenu( "File" ) )
      {
        if ( ImGui::MenuItem( "Open model..." ) )
        {
          openFileDialog = true;
        }
        ImGui::Separator();
        if ( ImGui::MenuItem( "Exit" ) )
        {
          appWantsToQuit = true;
        }
        ImGui::EndMenu();
      }
      if ( ImGui::BeginMenu( "Model" ) )
      {
        ImGui::MenuItem( "Show model info", NULL, &showModelInfo );
        ImGui::Separator();

        bool xyzSpace = !xzySpace;
        if ( ImGui::MenuItem( "XYZ space", NULL, &xyzSpace ) )
        {
          xzySpace = !xzySpace;
        }
        ImGui::MenuItem( "XZY space", NULL, &xzySpace );
        ImGui::EndMenu();
      }
      if ( ImGui::BeginMenu( "View" ) )
      {
        ImGui::MenuItem( "Enable idle camera", NULL, &automaticCamera );
        ImGui::ColorEdit4( "Background", (float *) &clearColor, ImGuiColorEditFlags_AlphaPreviewHalf );
        ImGui::Separator();
        ImGui::DragFloat( "Sky blur", &skysphereBlur, 0.1f, 0.0f, 9.0f );
        ImGui::DragFloat( "Sky opacity", &skysphereOpacity, 0.02f, 0.0f, 1.0f );
#ifdef _DEBUG
        ImGui::Separator();
        ImGui::DragFloat( "Camera Yaw", &cameraYaw, 0.01f );
        ImGui::DragFloat( "Camera Pitch", &cameraPitch, 0.01f );
        ImGui::DragFloat3( "Camera Target", (float *) &gCameraTarget );
#endif
        ImGui::EndMenu();
      }
      if ( ImGui::BeginMenu( "Shaders" ) )
      {
        for ( int i = 0; i< options.get<jsonxx::Array>( "shaders" ).size(); i++ )
        {
          const jsonxx::Object & shaderConfig = options.get<jsonxx::Array>( "shaders" ).get<jsonxx::Object>( i );
          const std::string & name = options.get<jsonxx::Array>( "shaders" ).get<jsonxx::Object>( i ).get<jsonxx::String>( "name" );

          bool selected = &shaderConfig == gCurrentShaderConfig;
          if ( ImGui::MenuItem( name.c_str(), NULL, &selected ) )
          {
            LoadShaderConfig( &shaderConfig );
          }
          gModel.RebindVertexArray( gCurrentShader );
        }
        if ( gCurrentShaderConfig && gCurrentShaderConfig->get<jsonxx::Boolean>("showSkybox") )
        {
          ImGui::Separator();
          for ( int i = 0; i < options.get<jsonxx::Array>( "skyImages" ).size(); i++ )
          {
            const auto & images = options.get<jsonxx::Array>( "skyImages" ).get<jsonxx::Object>( i );
            const std::string & filename = images.get<jsonxx::String>( "reflection" );

            bool selected = ( gSkyImages.reflection && gSkyImages.reflection->mFilename == filename );
            if ( ImGui::MenuItem( filename.c_str(), NULL, &selected ) )
            {
              loadSkyImages(
                images.get<jsonxx::String>( "reflection" ).c_str(),
                images.has<jsonxx::String>( "env" ) ? images.get<jsonxx::String>( "env" ).c_str() : NULL );
            }
          }
        }
        ImGui::EndMenu();
      }

      ImGui::EndMainMenuBar();
    }

    if ( openFileDialog )
    {
      ImGui::OpenPopup( "Open model" );
    }

    if ( file_dialog.showFileDialog( "Open model", imgui_addons::ImGuiFileBrowser::DialogMode::OPEN, ImVec2( 700, 310 ), supportedExtensions.c_str() ) )
    {
      LoadMesh( file_dialog.selected_path.c_str() );
    }

    if ( showModelInfo )
    {
      ImGui::Begin( "Model info", &showModelInfo );
      ImGui::BeginTabBar( "model" );
      if ( ImGui::BeginTabItem( "Summary" ) )
      {
        int triCount = 0;
        for ( std::map<int, Geometry::Mesh>::iterator it = gModel.mMeshes.begin(); it != gModel.mMeshes.end(); it++ )
        {
          triCount += it->second.mTriangleCount;
        }

        ImGui::Text( "Triangle count: %d", triCount );
        ImGui::Text( "Mesh count: %d", gModel.mMeshes.size() );

        ImGui::EndTabItem();
      }
      if ( ImGui::BeginTabItem( "Node tree" ) )
      {
        ShowNodeInImGui( -1 );

        ImGui::EndTabItem();
      }
      if ( ImGui::BeginTabItem( "Textures / Materials" ) )
      {
        ImGui::Text( "Material count: %d", gModel.mMaterials.size() );

        for ( std::map<int, Geometry::Material>::iterator it = gModel.mMaterials.begin(); it != gModel.mMaterials.end(); it++ )
        {
          if ( ImGui::CollapsingHeader( it->second.mName.c_str() ) )
          {
            ImGui::Indent();
            ImGui::Text( "Specular shininess: %g", it->second.mSpecularShininess );
            ImGui::ColorEdit4( "Ambient color", (float *) &it->second.mColorAmbient, ImGuiColorEditFlags_AlphaPreviewHalf );
            ImGui::ColorEdit4( "Diffuse color", (float *) &it->second.mColorDiffuse, ImGuiColorEditFlags_AlphaPreviewHalf );
            ImGui::ColorEdit4( "Specular color", (float *) &it->second.mColorSpecular, ImGuiColorEditFlags_AlphaPreviewHalf );
            if ( ImGui::BeginTabBar( it->second.mName.c_str() ) )
            {
              ShowMaterialInImGui( "Diffuse", it->second.mTextureDiffuse );
              ShowMaterialInImGui( "Normals", it->second.mTextureNormals );
              ShowMaterialInImGui( "Specular", it->second.mTextureSpecular );
              ShowMaterialInImGui( "Albedo", it->second.mTextureAlbedo );
              ShowMaterialInImGui( "Metallic", it->second.mTextureMetallic );
              ShowMaterialInImGui( "Roughness", it->second.mTextureRoughness );
              ShowMaterialInImGui( "AO", it->second.mTextureAO );
              ImGui::EndTabBar();
            }
            ImGui::Unindent();
          }
        }

        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
      ImGui::End();
    }

    ImGui::Render();

    //////////////////////////////////////////////////////////////////////////
    // Drag'n'drop

    for ( int i = 0; i < Renderer::dropEventBufferCount; i++ )
    {
      std::string & path = Renderer::dropEventBuffer[ i ];
      LoadMesh( path.c_str() );
    }
    Renderer::dropEventBufferCount = 0;

    //////////////////////////////////////////////////////////////////////////
    // Mouse rotation

    if ( !io.WantCaptureMouse )
    {
      for ( int i = 0; i < Renderer::mouseEventBufferCount; i++ )
      {
        Renderer::MouseEvent & mouseEvent = Renderer::mouseEventBuffer[ i ];
        switch ( mouseEvent.eventType )
        {
          case Renderer::MOUSEEVENTTYPE_MOVE:
            {
              const float rotationSpeed = 130.0f;
              if ( movingCamera )
              {
                cameraYaw -= ( mouseEvent.x - mouseClickPosX ) / rotationSpeed;
                cameraPitch += ( mouseEvent.y - mouseClickPosY ) / rotationSpeed;

                // Clamp to avoid gimbal lock
                cameraPitch = std::min( cameraPitch, 1.5f );
                cameraPitch = std::max( cameraPitch, -1.5f );
              }
              if ( movingLight )
              {
                lightYaw += ( mouseEvent.x - mouseClickPosX ) / rotationSpeed;
                lightPitch -= ( mouseEvent.y - mouseClickPosY ) / rotationSpeed;

                // Clamp to avoid gimbal lock
                lightPitch = std::min( lightPitch, 1.5f );
                lightPitch = std::max( lightPitch, -1.5f );
              }
              mouseClickPosX = mouseEvent.x;
              mouseClickPosY = mouseEvent.y;
            }
            break;
          case Renderer::MOUSEEVENTTYPE_DOWN:
            {
              if ( mouseEvent.button == Renderer::MOUSEBUTTON_LEFT )
              {
                movingCamera = true;
                automaticCamera = false;
              }
              else if ( mouseEvent.button == Renderer::MOUSEBUTTON_RIGHT )
              {
                movingLight = true;
              }
              mouseClickPosX = mouseEvent.x;
              mouseClickPosY = mouseEvent.y;
            }
            break;
          case Renderer::MOUSEEVENTTYPE_UP:
            {
              if ( mouseEvent.button == Renderer::MOUSEBUTTON_LEFT )
              {
                movingCamera = false;
              }
              else if ( mouseEvent.button == Renderer::MOUSEBUTTON_RIGHT )
              {
                movingLight = false;
              }
            }
            break;
          case Renderer::MOUSEEVENTTYPE_SCROLL:
            {
              const float aspect = 1.1f;
              gCameraDistance *= mouseEvent.y < 0 ? aspect : 1 / aspect;
            }
            break;
        }
      }
    }
    Renderer::mouseEventBufferCount = 0;

    //////////////////////////////////////////////////////////////////////////
    // Camera and lights

    if ( automaticCamera )
    {
      cameraYaw += io.DeltaTime * 0.3f;
    }

    //////////////////////////////////////////////////////////////////////////
    // Skysphere render

    glm::vec3 cameraPosition( 0.0f, 0.0f, -1.0f );
    cameraPosition = glm::rotateX( cameraPosition, cameraPitch );
    cameraPosition = glm::rotateY( cameraPosition, cameraYaw );

    static glm::mat4x4 worldRootXYZ( 1.0f );
    if ( gCurrentShaderConfig->get<jsonxx::Boolean>( "showSkybox" ) )
    {
      float verticalFovInRadian = 0.5f;
      projectionMatrix = glm::perspective( verticalFovInRadian, settings.nWidth / (float) settings.nHeight, 0.001f, 2.0f );
      skysphereShader->SetConstant( "mat_projection", projectionMatrix );

      viewMatrix = glm::lookAtRH( cameraPosition * 0.15f, glm::vec3( 0.0f, 0.0f, 0.0f ), glm::vec3( 0.0f, 1.0f, 0.0f ) );
      skysphereShader->SetConstant( "mat_view", viewMatrix );

      skysphereShader->SetConstant( "has_tex_skysphere", gSkyImages.reflection != NULL );
      if ( gSkyImages.reflection )
      {
        skysphereShader->SetTexture( "tex_skysphere", gSkyImages.reflection );
      }

      skysphereShader->SetConstant( "background_color", clearColor );
      skysphereShader->SetConstant( "skysphere_blur", skysphereBlur );
      skysphereShader->SetConstant( "skysphere_opacity", skysphereOpacity );
      skysphereShader->SetConstant( "skysphere_rotation", lightYaw );

      skysphere.Render( worldRootXYZ, skysphereShader );

      glClear( GL_DEPTH_BUFFER_BIT );
    }

    //////////////////////////////////////////////////////////////////////////
    // Mesh render

    float verticalFovInRadian = 0.5f;
    projectionMatrix = glm::perspective( verticalFovInRadian, settings.nWidth / (float) settings.nHeight, gCameraDistance / 1000.0f, gCameraDistance * 2.0f );
    gCurrentShader->SetConstant( "mat_projection", projectionMatrix );

    cameraPosition *= gCameraDistance;
    gCurrentShader->SetConstant( "camera_position", cameraPosition );

    glm::vec3 lightDirection( 0.0f, 0.0f, 1.0f );
    lightDirection = glm::rotateX( lightDirection, lightPitch );
    lightDirection = glm::rotateY( lightDirection, lightYaw );

    glm::vec3 fillLightDirection( 0.0f, 0.0f, 1.0f );
    fillLightDirection = glm::rotateX( fillLightDirection, lightPitch - 0.4f );
    fillLightDirection = glm::rotateY( fillLightDirection, lightYaw + 0.8f );

    gCurrentShader->SetConstant( "lights[0].direction", lightDirection );
    gCurrentShader->SetConstant( "lights[0].color", glm::vec3( 1.0f ) );
    gCurrentShader->SetConstant( "lights[1].direction", fillLightDirection );
    gCurrentShader->SetConstant( "lights[1].color", glm::vec3( 0.5f ) );
    gCurrentShader->SetConstant( "lights[2].direction", -fillLightDirection );
    gCurrentShader->SetConstant( "lights[2].color", glm::vec3( 0.25f ) );

    gCurrentShader->SetConstant( "skysphere_rotation", lightYaw );

    viewMatrix = glm::lookAtRH( cameraPosition + gCameraTarget, gCameraTarget, glm::vec3( 0.0f, 1.0f, 0.0f ) );
    gCurrentShader->SetConstant( "mat_view", viewMatrix );
    gCurrentShader->SetConstant( "mat_view_inverse", glm::inverse( viewMatrix ) );

    gCurrentShader->SetConstant( "has_tex_skysphere", gSkyImages.reflection != NULL );
    gCurrentShader->SetConstant( "has_tex_skyenv", gSkyImages.env != NULL );
    if ( gSkyImages.reflection )
    {
      float mipCount = floor( log2( gSkyImages.reflection->mHeight ) );
      gCurrentShader->SetTexture( "tex_skysphere", gSkyImages.reflection );
      gCurrentShader->SetConstant( "skysphere_mip_count", mipCount );
    }
    if ( gSkyImages.env )
    {
      gCurrentShader->SetTexture( "tex_skyenv", gSkyImages.env );
    }
    gCurrentShader->SetTexture( "tex_brdf_lut", gBrdfLookupTable );

    //////////////////////////////////////////////////////////////////////////
    // Mesh render

    gModel.Render( xzySpace ? xzyMatrix : worldRootXYZ, gCurrentShader );

    //////////////////////////////////////////////////////////////////////////
    // End frame
    ImGui_ImplOpenGL3_RenderDrawData( ImGui::GetDrawData() );
    Renderer::EndFrame();
  }

  //////////////////////////////////////////////////////////////////////////
  // Cleanup

  if ( gCurrentShader )
  {
    Renderer::ReleaseShader( gCurrentShader );
    delete gCurrentShader;
  }
  if ( skysphereShader )
  {
    Renderer::ReleaseShader( skysphereShader );
    delete skysphereShader;
  }
  if ( gSkyImages.reflection )
  {
    Renderer::ReleaseTexture( gSkyImages.reflection );
    gSkyImages.reflection = NULL;
  }
  if ( gSkyImages.env )
  {
    Renderer::ReleaseTexture( gSkyImages.env );
    gSkyImages.env = NULL;
  }

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();

  gModel.UnloadMesh();

  Renderer::Close();

  return 0;
}